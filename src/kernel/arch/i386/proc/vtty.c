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

static int      vtty_nslots  = 0;
static int      vtty_current = 0;
static vt_buf_t vtty_bufs[VTTY_MAX];
static bool     vtty_bufs_ready = false;
static volatile int vtty_open_requests = 0;
static volatile int vtty_clock_toggle_requests = 0;

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
    bool reserve_status = false;
    if (vesa_tty_is_ready()) {
        cols = vesa_tty_get_cols();
        rows = vesa_tty_get_rows();
        if (rows > VESA_TTY_STATUS_ROWS) {
            rows -= VESA_TTY_STATUS_ROWS;
            reserve_status = true;
        }
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
        /* Root slot (mak.sh0) gets full tty height so text and scrolling
         * can reach the bottom row when no status bar is visible.
         * Makmux slots 0-3 use the status-bar-reserved height. */
        uint32_t slot_rows = (i == VTTY_ROOT_SLOT) ? vesa_tty_get_rows() : rows;
        if (!vt_init(&vtty_bufs[i], cols, slot_rows,
                     VTTY_DEFAULT_FG, VTTY_DEFAULT_BG)) {
            /* Allocation failed: leave cells == NULL so vt_putchar
             * becomes a no-op for that slot.  Better than panic at boot. */
            vtty_bufs[i].cells = NULL;
        }
    }

    (void)reserve_status;
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
    int slot = -1;
    for (int i = 0; i < VTTY_MAX - 1; i++) {   /* -1: skip root slot */
        if (!vtty_owner(i)) { slot = i; break; }
    }
    if (slot < 0) return -1;
    if (slot >= vtty_nslots) vtty_nslots = slot + 1;
    task_t *me = task_current();
    if (me) me->tty = slot;
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

    vt_buf_t *vt = vtty_buf(slot);
    if (vt) vt_clear(vt);

    while (vtty_nslots > 0 && !vtty_owner(vtty_nslots - 1))
        vtty_nslots--;

    if (vtty_nslots == 0) {
        vtty_current = 0;
        keyboard_set_focus(task_current());
        /* Erase the stale makmux status bar row now that no VT children
         * remain; the root slot's vtty_request_repaint (from SYS_WAIT4
         * when mak.sh0 reaps makmux) will repaint the text area above it. */
        vesa_tty_set_status_visible(0);
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
