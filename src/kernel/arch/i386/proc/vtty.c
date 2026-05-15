/*
 * vtty.c - virtual TTY manager.
 *
 * Each slot binds one shell task.  Alt+F1-F4 (handled in keyboard.c) calls
 * vtty_switch() to change the active slot, update keyboard routing, and send
 * KEY_FOCUS_GAIN to the newly focused task so it can redraw.
 *
 * `task_t.tty` is authoritative for the TTY a task is bound to.  vtty itself
 * only tracks the focused slot index and the number of registered slots; the
 * "owning task" of slot N is resolved by walking the task pool for the live
 * task with the lowest pid whose `tty == N` (the original shell — exec
 * children inherit the same tty but always have higher pids).
 */

#include <kernel/vtty.h>
#include <kernel/keyboard.h>

static int vtty_nslots  = 0;
static int vtty_current = 0;

void vtty_init(void)
{
    vtty_nslots  = 0;
    vtty_current = 0;
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

void vtty_switch(int n)
{
    if (n < 0 || n >= vtty_nslots || n == vtty_current) return;
    task_t *owner = vtty_owner(n);
    if (!owner) return;
    vtty_current = n;
    keyboard_set_focus(owner);
    keyboard_send_to(owner, KEY_FOCUS_GAIN);
}
