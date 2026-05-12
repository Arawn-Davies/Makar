/*
 * vtty.c - virtual TTY manager.
 *
 * Each slot holds one shell task.  Alt+F1-F4 (handled in keyboard.c) calls
 * vtty_switch() to change the active slot, update keyboard routing, and send
 * KEY_FOCUS_GAIN to the newly focused task so it can redraw.
 */

#include <kernel/vtty.h>
#include <kernel/keyboard.h>

static task_t *vtty_tasks[VTTY_MAX];
static int     vtty_nslots  = 0;
static int     vtty_current = 0;

void vtty_init(void)
{
    for (int i = 0; i < VTTY_MAX; i++)
        vtty_tasks[i] = NULL;
    vtty_nslots  = 0;
    vtty_current = 0;
}

int vtty_register(void)
{
    if (vtty_nslots >= VTTY_MAX) return -1;
    int slot = vtty_nslots++;
    vtty_tasks[slot] = task_current();
    if (slot == 0)
        keyboard_set_focus(vtty_tasks[0]);
    return slot;
}

int vtty_active(void)
{
    return vtty_current;
}

int vtty_is_focused(void)
{
    return vtty_tasks[vtty_current] == task_current();
}

int vtty_count(void)
{
    return vtty_nslots;
}

void vtty_switch(int n)
{
    if (n < 0 || n >= vtty_nslots || n == vtty_current) return;
    vtty_current = n;
    keyboard_set_focus(vtty_tasks[n]);
    keyboard_send_to(vtty_tasks[n], KEY_FOCUS_GAIN);
}
