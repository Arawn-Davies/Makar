/*
 * vtty.c - virtual TTY manager.
 *
 * Each slot binds one shell task.  Alt+F1-F4 (handled in keyboard.c) calls
 * vtty_switch() to change the active slot, update keyboard routing, and send
 * KEY_FOCUS_GAIN to the newly focused task so it can redraw.
 *
 * `task_t.tty` is authoritative for the TTY a task is bound to.  vtty itself
 * tracks the focused slot index, the number of registered slots, and the
 * per-slot backing grid (vt_buf_t).  The "owning task" of slot N is resolved
 * by walking the task pool for the live task with the lowest pid whose
 * `tty == N` (the original shell -- exec children inherit the same tty but
 * always have higher pids).
 *
 * Per-TTY backing grids hold all output the shell / ring-3 apps emit; the
 * display renderer (vesa_tty / VGA tty) writes through the current task's
 * buffer and only mirrors to the physical framebuffer when that buffer
 * matches the focused TTY.  Switching TTYs re-paints the new buffer's grid
 * back to the framebuffer so the background shell's state is preserved.
 */

#include <kernel/vtty.h>
#include <kernel/keyboard.h>
#include <kernel/vesa_tty.h>
#include <kernel/heap.h>
#include <kernel/atomic.h>
#include <string.h>

static int      vtty_nslots  = 0;
static int      vtty_current = 0;
static vt_buf_t vtty_bufs[VTTY_MAX];
static bool     vtty_bufs_ready = false;
static volatile int vtty_open_requests = 0;
static volatile int vtty_clock_toggle_requests = 0;

/* Per-slot tab name (app-tabs: "maktop"/"vix"/...; empty = default "VTn"). */
#define VTTY_NAME_MAX 16
static char vtty_names[VTTY_MAX][VTTY_NAME_MAX];

/* App-tab launch queue: shell -> kernel (vtty_open_app) -> makmux (drains and
 * forks a VT child that execs the app).  Small ring of request paths. */
#define VTTY_APP_PATH 128
#define VTTY_APP_Q    4
static char vtty_app_q[VTTY_APP_Q][VTTY_APP_PATH];
static volatile int vtty_app_head = 0;
static volatile int vtty_app_tail = 0;

/* Pending repaint target.  vtty_switch runs in the keyboard IRQ; doing a
 * full framebuffer repaint there would block subsequent keyboard IRQs
 * long enough that the i8042's 1-byte OBF stays full and the edge-
 * triggered PIC misses key events (the same failure mode as PR #127).
 * Mark the target instead; vtty_drain_pending() flushes the paint from
 * task context the next time the new owner's REPL yields. */
static volatile int vtty_pending = -1;

/* Per-slot foreground task override.  When non-NULL, vtty_switch
 * directs keyboard focus + KEY_FOCUS_GAIN to this task instead of
 * the slot's registered shell.  Used by shell_exec_elf so the
 * exec'd child keeps getting keystrokes even after the user
 * Alt+Fn'd away and back.  Without it: after a VT excursion the
 * shell got refocused, the foreground task (maktop, vix) sat in
 * its read loop with no keys arriving, the operator had to kill
 * the QEMU window. */
static task_t * volatile vtty_foreground[VTTY_MAX];

/* Default attribute values used when no display renderer has set colours
 * yet.  Renderer interprets - VESA uses these as composed FB pixels,
 * VGA uses only the low byte as a VGA attribute. */
#define VTTY_DEFAULT_FG  0xFFFFFFFFu
#define VTTY_DEFAULT_BG  0x00000000u

/* Bottom-row status bar reservation lives in <kernel/vesa_tty.h> as
 * VESA_TTY_STATUS_ROWS - one source of truth shared with the framebuffer
 * renderer and with VIX. */

void vtty_init(void)
{
    vtty_nslots  = 0;
    vtty_current = 0;

    uint32_t cols = 0, rows = 0;
    if (vesa_tty_is_ready()) {
        cols = vesa_tty_get_cols();
        rows = vesa_tty_get_rows();
    }
    if (cols == 0 || rows == 0) {
        cols = 80;
        rows = 50;
    }

    /* Idempotent: free any pre-existing cell allocations so re-running
     * vtty_init after a setmode (which changes tty geometry) resizes
     * the buffers rather than leaking the old grids. */
    for (int i = 0; i < VTTY_MAX; i++) {
        if (vtty_bufs[i].cells) {
            kfree(vtty_bufs[i].cells);
            vtty_bufs[i].cells = NULL;
        }
    }

    vtty_bufs_ready = true;
    for (int i = 0; i < VTTY_MAX; i++) {
        /* Allocate every VT at physical height, then let vt->usable_rows
         * define the active console scroll region.  That mirrors Linux's
         * split between screen capacity and the current visible/scroll area:
         * enabling the status bar reserves the last row, disabling it gives
         * that row back without reallocating each VT buffer. */
        if (!vt_init(&vtty_bufs[i], cols, rows,
                     VTTY_DEFAULT_FG, VTTY_DEFAULT_BG)) {
            /* Allocation failed: leave cells == NULL so vt_putchar
             * becomes a no-op for that slot.  Better than panic at boot. */
            vtty_bufs[i].cells = NULL;
        } else if (vesa_tty_is_ready()) {
            vt_set_usable_rows(&vtty_bufs[i], vesa_tty_usable_rows());
        }
    }
}

/*
 * vtty_owner - find the registered shell task for slot n.
 * Returns the live task with t->tty == n and the lowest pid, or NULL if
 * no such task exists (slot vacated by death).
 */
static task_t *vtty_owner(int n)
{
    task_t *best = NULL;
    int     best_pid = 0;
    for (int i = 0; i < task_count(); i++) {
        task_t *t = task_get(i);
        if (!t || t->state == TASK_DEAD) continue;
        if (t->tty != n) continue;
        if (!best || t->pid < best_pid) {
            best     = t;
            best_pid = t->pid;
        }
    }
    return best;
}

int vtty_register(void)
{
    task_t *me = task_current();
    int slot = -1;

    /* Claim the lowest free slot ATOMICALLY.  makmux forks all four VT
     * children before yielding, and syscalls run with interrupts enabled
     * (sti in isr_asm.S), so the PIT can preempt this task between the
     * vtty_owner() "is it free?" check and the me->tty assignment that marks
     * it taken.  Without the IF guard, two children both read the same slot
     * as free and both claim it -- producing two shells on slot 0 (the
     * "duplicate mak.sh1" bug).  cli/sti makes find-then-claim indivisible. */
    uint32_t flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    for (int i = 0; i < VTTY_MAX - 1; i++) {   /* -1: skip root slot */
        if (!vtty_owner(i)) { slot = i; break; }
    }
    if (slot >= 0) {
        if (me) me->tty = slot;
        if (slot >= vtty_nslots) vtty_nslots = slot + 1;
    }
    asm volatile("pushl %0; popfl" :: "r"(flags) : "memory", "cc");

    if (slot < 0) return -1;
    if (slot == 0)
        keyboard_set_focus(me);
    vt_buf_t *vt = vtty_buf(slot);
    if (vt) vt_clear(vt);
    return slot;
}

int vtty_register_root(void)
{
    task_t *me = task_current();
    if (!me) return -1;
    me->tty = VTTY_ROOT_SLOT;
    /* Root slot is focused whenever vtty_nslots == 0; set keyboard focus
     * now since mak.sh0 starts before any makmux VTs exist. */
    keyboard_set_focus(me);
    vt_buf_t *vt = vtty_buf(VTTY_ROOT_SLOT);
    if (vt) vt_clear(vt);
    return VTTY_ROOT_SLOT;
}

int vtty_close_pid(int pid)
{
    int slot = -1;
    for (int i = 0; i < task_count(); i++) {
        task_t *t = task_get(i);
        if (!t || t->pid != pid) continue;
        slot = t->tty;
        t->tty = TASK_TTY_NONE;
        break;
    }
    if (slot < 0 || slot >= VTTY_MAX || slot == VTTY_ROOT_SLOT) return -1;

    for (int i = 0; i < task_count(); i++) {
        task_t *t = task_get(i);
        if (t && t->tty == slot)
            t->tty = TASK_TTY_NONE;
    }
    __atomic_store_n(&vtty_foreground[slot], (task_t *)0, __ATOMIC_RELEASE);
    vtty_names[slot][0] = '\0';            /* drop the app-tab name */

    vt_buf_t *vt = vtty_buf(slot);
    if (vt) vt_clear(vt);

    while (vtty_nslots > 0 && !vtty_owner(vtty_nslots - 1))
        vtty_nslots--;

    if (vtty_nslots == 0) {
        vtty_current = 0;
        keyboard_set_focus(task_current());
        /* No VT children left: the status bar stays enabled (it just stops
         * drawing tabs since vtty_count() == 0).  Restore mak.sh0's screen --
         * the VT children/app-tabs drew over the framebuffer, and makmux
         * itself no longer paints (so its wait4 reaper can't), so repaint the
         * root console's backing buffer when the last VT closes.  mak.sh0 owns
         * VTTY_ROOT_SLOT, so its next keyboard_getchar drains this repaint. */
        vtty_request_repaint(VTTY_ROOT_SLOT);
    } else if (vtty_current == slot || vtty_current >= vtty_nslots) {
        int next = -1;
        for (int i = slot; i < vtty_nslots; i++) {
            if (vtty_owner(i)) { next = i; break; }
        }
        for (int i = slot - 1; next < 0 && i >= 0; i--) {
            if (vtty_owner(i)) { next = i; break; }
        }
        if (next < 0) next = 0;
        vtty_current = next;
        task_t *owner = vtty_owner(next);
        if (owner) {
            keyboard_set_focus(owner);
            keyboard_send_to(owner, KEY_FOCUS_GAIN);
            __atomic_store_n(&vtty_pending, next, __ATOMIC_RELEASE);
        } else {
            keyboard_set_focus(task_current());
        }
    }

    return slot;
}

void vtty_request_open(void)
{
    __atomic_fetch_add(&vtty_open_requests, 1, __ATOMIC_RELEASE);
}

int vtty_take_open_request(void)
{
    int n = __atomic_exchange_n(&vtty_open_requests, 0, __ATOMIC_ACQ_REL);
    return n;
}

void vtty_request_clock_toggle(void)
{
    __atomic_fetch_add(&vtty_clock_toggle_requests, 1, __ATOMIC_RELEASE);
}

int vtty_take_clock_toggle_request(void)
{
    int n = __atomic_exchange_n(&vtty_clock_toggle_requests, 0, __ATOMIC_ACQ_REL);
    return n;
}

/* ---- VT tab names + app-tab launch routing ----------------------------- */

void vtty_set_name(int slot, const char *name)
{
    if (slot < 0 || slot >= VTTY_MAX) return;
    int i = 0;
    for (; name && name[i] && i < VTTY_NAME_MAX - 1; i++)
        vtty_names[slot][i] = name[i];
    vtty_names[slot][i] = '\0';
}

const char *vtty_get_name(int slot)
{
    if (slot < 0 || slot >= VTTY_MAX) return "";
    return vtty_names[slot];
}

/* Slot of a *live* tab whose name matches, or -1. */
int vtty_find_name(const char *name)
{
    if (!name || !*name) return -1;
    for (int i = 0; i < VTTY_MAX; i++) {
        if (i == VTTY_ROOT_SLOT) continue;
        if (vtty_owner(i) && strcmp(vtty_names[i], name) == 0)
            return i;
    }
    return -1;
}

/* Derive a tab name from an ELF path: basename without ".elf". */
static void app_name_from_path(const char *path, char *out, int cap)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') base = p + 1;
    int n = 0;
    while (base[n] && n < cap - 1) { out[n] = base[n]; n++; }
    out[n] = '\0';
    if (n >= 4 && out[n-4] == '.' && out[n-3] == 'e' &&
        out[n-2] == 'l' && out[n-1] == 'f')
        out[n-4] = '\0';
}

/* Shell entry point (SYS_VT_OPEN_APP): open `path` in a named app-tab.  If a
 * live tab with that name already exists, just switch to it; otherwise queue
 * the path for makmux to fork + exec into a fresh slot.  Returns 1 if it was
 * switched/queued, 0 if the queue was full. */
int vtty_open_app(const char *path)
{
    if (!path || !*path) return 0;
    char name[VTTY_NAME_MAX];
    app_name_from_path(path, name, sizeof(name));

    int slot = vtty_find_name(name);
    if (slot >= 0) { vtty_switch(slot); return 1; }

    int next = (vtty_app_head + 1) % VTTY_APP_Q;
    if (next == vtty_app_tail) return 0;            /* queue full */
    int i = 0;
    for (; path[i] && i < VTTY_APP_PATH - 1; i++) vtty_app_q[vtty_app_head][i] = path[i];
    vtty_app_q[vtty_app_head][i] = '\0';
    __atomic_store_n(&vtty_app_head, next, __ATOMIC_RELEASE);
    return 1;
}

/* makmux drains one queued app path; returns 1 if one was written to out. */
int vtty_take_app_request(char *out, int cap)
{
    int tail = __atomic_load_n(&vtty_app_tail, __ATOMIC_ACQUIRE);
    if (tail == __atomic_load_n(&vtty_app_head, __ATOMIC_ACQUIRE)) return 0;
    int i = 0;
    for (; vtty_app_q[tail][i] && i < cap - 1; i++) out[i] = vtty_app_q[tail][i];
    out[i] = '\0';
    __atomic_store_n(&vtty_app_tail, (tail + 1) % VTTY_APP_Q, __ATOMIC_RELEASE);
    return 1;
}

int vtty_active(void)
{
    return vtty_current;
}

int vtty_is_focused(void)
{
    task_t *me = task_current();
    if (!me) return 0;
    /* Root slot is focused whenever no makmux VT children are registered. */
    if (me->tty == VTTY_ROOT_SLOT)
        return vtty_nslots == 0;
    return me->tty >= 0 && me->tty == vtty_current;
}

int vtty_count(void)
{
    return vtty_nslots;
}

void vtty_request_repaint(int slot)
{
    if (slot < 0 || slot >= VTTY_MAX) return;
    __atomic_store_n(&vtty_pending, slot, __ATOMIC_RELEASE);
}

unsigned int vtty_live_mask(void)
{
    unsigned int mask = 0;
    for (int i = 0; i < VTTY_MAX; i++) {
        if (i == VTTY_ROOT_SLOT) continue;   /* hidden console, not user-visible */
        if (vtty_owner(i))
            mask |= (1u << i);
    }
    return mask;
}

vt_buf_t *vtty_buf(int n)
{
    if (!vtty_bufs_ready || n < 0 || n >= VTTY_MAX) return NULL;
    return &vtty_bufs[n];
}

vt_buf_t *vtty_buf_current(void)
{
    if (!vtty_bufs_ready) return NULL;
    task_t *me = task_current();
    if (!me) return NULL;
    int n = me->tty;
    if (n < 0 || n >= VTTY_MAX) return NULL;
    return &vtty_bufs[n];
}

vt_buf_t *vtty_buf_focused(void)
{
    /* When no makmux VTs exist the root slot is the active display. */
    if (vtty_nslots == 0)
        return vtty_buf(VTTY_ROOT_SLOT);
    return vtty_buf(vtty_current);
}

void vtty_switch(int n)
{
    if (n < 0 || n >= vtty_nslots || n == vtty_current || n == VTTY_ROOT_SLOT) return;
    /* Foreground task override beats the slot's shell.  Falls back to
     * the slot owner when no fullscreen child is currently running. */
    task_t *fg    = __atomic_load_n(&vtty_foreground[n], __ATOMIC_ACQUIRE);
    task_t *owner = fg ? fg : vtty_owner(n);
    if (!owner) return;
    vtty_current = n;
    keyboard_set_focus(owner);
    keyboard_send_to(owner, KEY_FOCUS_GAIN);
    /* Defer the FB repaint to task context - see vtty_pending comment. */
    __atomic_store_n(&vtty_pending, n, __ATOMIC_RELEASE);
}

void vtty_set_foreground(int slot, task_t *t)
{
    if (slot < 0 || slot >= VTTY_MAX) return;
    __atomic_store_n(&vtty_foreground[slot], t, __ATOMIC_RELEASE);
}

void vtty_drain_pending(void)
{
    int n = __atomic_load_n(&vtty_pending, __ATOMIC_ACQUIRE);
    if (n < 0) return;

    /* Only the task that owns the destination TTY repaints.  Otherwise
     * a non-focused shell calling keyboard_getchar (sitting idle on its
     * own slot) would race with the destination shell writing the
     * prompt / accumulated content to vt_buf[n] AND directly to the FB
     * via the focused-write path.  That race produced visible artefacts
     * (stray caret glyphs in the middle of the screen) in interactive
     * Alt+Fn switches. */
    task_t *me = task_current();
    if (!me || me->tty != n) return;

    /* Compare-exchange so concurrent owners (parent shell + exec child
     * sharing the same TTY) don't double-paint. */
    int expected = n;
    if (!__atomic_compare_exchange_n(&vtty_pending, &expected, -1,
                                     /*weak=*/0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;

    /* Always repaint the destination VT's text backing grid so its prior
     * content and colour scheme are restored, and any raw pixels a
     * fullscreen app left on the framebuffer are covered (keeping each
     * VT's display isolated).  A continuously-drawing app (e.g. basic's
     * lines) repaints its own graphics on the next frame; a one-shot
     * graphics frame is intentionally not preserved across a switch.
     * The pending flag was CAS'd to -1 above, so this fires exactly once
     * per switch -- not on every yield. */
    vt_buf_t *vt = vtty_buf(n);
    if (vt) vesa_tty_paint_buf(vt);
}
