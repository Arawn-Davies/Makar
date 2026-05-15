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

static int      vtty_nslots  = 0;
static int      vtty_current = 0;
static vt_buf_t vtty_bufs[VTTY_MAX];
static bool     vtty_bufs_ready = false;

/* Pending repaint target.  vtty_switch runs in the keyboard IRQ; doing a
 * full framebuffer repaint there would block subsequent keyboard IRQs
 * long enough that the i8042's 1-byte OBF stays full and the edge-
 * triggered PIC misses key events (the same failure mode as PR #127).
 * Mark the target instead; vtty_drain_pending() flushes the paint from
 * task context the next time the new owner's REPL yields. */
static volatile int vtty_pending = -1;

/* Default attribute values used when no display renderer has set colours
 * yet.  Renderer interprets - VESA uses these as composed FB pixels,
 * VGA uses only the low byte as a VGA attribute. */
#define VTTY_DEFAULT_FG  0xFFFFFFFFu
#define VTTY_DEFAULT_BG  0x00000000u

/* Bottom-row status bar reservation.  When VESA is active we steal the
 * last character row so the tmux-style indicator can live there without
 * being scrolled away by shell output.  In VGA-text fallback the bar
 * isn't drawn (yet); the full 80x50 grid stays available. */
#define VTTY_STATUS_ROWS 1

void vtty_init(void)
{
    vtty_nslots  = 0;
    vtty_current = 0;

    uint32_t cols = 0, rows = 0;
    bool reserve_status = false;
    if (vesa_tty_is_ready()) {
        cols = vesa_tty_get_cols();
        rows = vesa_tty_get_rows();
        if (rows > VTTY_STATUS_ROWS) {
            rows -= VTTY_STATUS_ROWS;
            reserve_status = true;
        }
    }
    if (cols == 0 || rows == 0) {
        cols = 80;
        rows = 50;
    }

    vtty_bufs_ready = true;
    for (int i = 0; i < VTTY_MAX; i++) {
        if (!vt_init(&vtty_bufs[i], cols, rows,
                     VTTY_DEFAULT_FG, VTTY_DEFAULT_BG)) {
            /* Allocation failed: leave cells == NULL so vt_putchar
             * becomes a no-op for that slot.  Better than panic at boot. */
            vtty_bufs[i].cells = NULL;
        }
    }

    /* Initial status bar.  count=0 - no slots registered yet, but draw
     * the row so the dark band is visible during boot. */
    if (reserve_status)
        vesa_tty_paint_status(0, 0);
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
    if (vtty_nslots >= VTTY_MAX) return -1;
    int slot = vtty_nslots++;
    task_t *me = task_current();
    if (me) me->tty = slot;
    if (slot == 0)
        keyboard_set_focus(me);
    /* Refresh the status bar so the new slot lights up. */
    vesa_tty_paint_status(vtty_current, vtty_nslots);
    return slot;
}

int vtty_active(void)
{
    return vtty_current;
}

int vtty_is_focused(void)
{
    task_t *me = task_current();
    return me && me->tty == vtty_current;
}

int vtty_count(void)
{
    return vtty_nslots;
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
    return vtty_buf(vtty_current);
}

void vtty_switch(int n)
{
    if (n < 0 || n >= vtty_nslots || n == vtty_current) return;
    task_t *owner = vtty_owner(n);
    if (!owner) return;
    vtty_current = n;
    keyboard_set_focus(owner);
    keyboard_send_to(owner, KEY_FOCUS_GAIN);
    /* Defer the FB repaint to task context - see vtty_pending comment. */
    __atomic_store_n(&vtty_pending, n, __ATOMIC_RELEASE);
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

    vt_buf_t *vt = vtty_buf(n);
    if (vt) vesa_tty_paint_buf(vt);
    /* paint_buf only walks vt->rows, which excludes the status row, so
     * the bar survives a focus switch.  Repaint it anyway to update the
     * "active" highlight. */
    vesa_tty_paint_status(vtty_current, vtty_nslots);
}
