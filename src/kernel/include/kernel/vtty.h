#ifndef _KERNEL_VTTY_H
#define _KERNEL_VTTY_H

/*
 * vtty - virtual TTY manager.
 *
 * Up to VTTY_MAX shell tasks run concurrently; Alt+F1-F4 switches the active
 * (focused) TTY.  Only the active TTY receives keyboard input and is expected
 * to render to the screen.  vtty_switch() is safe to call from IRQ context.
 */

#include <kernel/task.h>

#define VTTY_MAX 4

/* Call once before spawning shell tasks. */
void vtty_init(void);

/* Register the calling task as the next available TTY slot.
 * Returns the slot index (0-based), or -1 if full.
 * The first task to register (slot 0) becomes the initial focused TTY. */
int vtty_register(void);

/* Returns the currently active TTY index. */
int vtty_active(void);

/* Returns 1 if the calling task is the currently active TTY. */
int vtty_is_focused(void);

/* Switch active TTY to slot n.  Sets keyboard focus, sends KEY_FOCUS_GAIN
 * to the new TTY's input queue.  Safe to call from IRQ context. */
void vtty_switch(int n);

/* Returns the number of registered TTY slots. */
int vtty_count(void);

#endif /* _KERNEL_VTTY_H */
