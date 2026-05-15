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

/* Default attribute values used when no display renderer has set colours
 * yet.  Renderer interprets - VESA uses these as composed FB pixels,
 * VGA uses only the low byte as a VGA attribute. */
#define VTTY_DEFAULT_FG  0xFFFFFFFFu
#define VTTY_DEFAULT_BG  0x00000000u

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

    vtty_bufs_ready = true;
    for (int i = 0; i < VTTY_MAX; i++) {
        if (!vt_init(&vtty_bufs[i], cols, rows,
                     VTTY_DEFAULT_FG, VTTY_DEFAULT_BG)) {
            /* Allocation failed: leave cells == NULL so vt_putchar
             * becomes a no-op for that slot.  Better than panic at boot. */
            vtty_bufs[i].cells = NULL;
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
    if (vtty_nslots >= VTTY_MAX) return -1;
    int slot = vtty_nslots++;
    task_t *me = task_current();
    if (me) me->tty = slot;
    if (slot == 0)
        keyboard_set_focus(me);
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
}
