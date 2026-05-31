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
#include <kernel/vt.h>

/* Slots 0..VTTY_MAX-2 are the user-visible VT slots (makmux VT1-4).
 * Slot VTTY_ROOT_SLOT is the hidden "console" slot for mak.sh0: it has a
 * full backing buffer (so its content is preserved across makmux sessions)
 * but it is excluded from Ctrl+Tab cycling, Alt+Fn switching, and the
 * makmux status bar — exactly like Linux's tty0/console. */
#define VTTY_MAX      5
#define VTTY_ROOT_SLOT 4

/* Call once before spawning shell tasks.  Allocates per-slot backing
 * grids sized from the active display geometry (VESA cell dims if the
 * framebuffer renderer is ready, otherwise the 80x50 VGA-text fallback). */
void vtty_init(void);

/* Register the calling task as the next available TTY slot.
 * Returns the slot index (0-based), or -1 if full.
 * The first task to register (slot 0) becomes the initial focused TTY. */
int vtty_register(void);

/* Register the calling task as the hidden root console (VTTY_ROOT_SLOT).
 * The root slot has a backing buffer but is excluded from Ctrl+Tab cycling,
 * Alt+Fn switching, vtty_live_mask(), and vtty_count() — it is never visible
 * in the makmux status bar.  The root slot is "focused" whenever no makmux
 * VT children exist (vtty_count() == 0). */
int vtty_register_root(void);

/* Close the VT owned by pid, clearing its backing grid and freeing the slot
 * for a future vtty_register().  Returns the closed slot, or -1. */
int vtty_close_pid(int pid);

/* Global Alt+T request queue consumed by makmux in userspace. */
void vtty_request_open(void);
int  vtty_take_open_request(void);

/* Global Alt+F5 request queue consumed by makmux in userspace. */
void vtty_request_clock_toggle(void);
int  vtty_take_clock_toggle_request(void);

/* Returns the currently active TTY index. */
int vtty_active(void);

/* Returns 1 if the calling task is the currently active TTY. */
int vtty_is_focused(void);

/* Switch active TTY to slot n.  Sets keyboard focus, sends KEY_FOCUS_GAIN
 * to the new TTY's input queue.  Safe to call from IRQ context. */
void vtty_switch(int n);

/* Returns the high-water number of registered TTY slots. */
int vtty_count(void);

/* Returns a bitmask of currently live TTY slots.  Bit 0 is VT1. */
unsigned int vtty_live_mask(void);

/* ------------------------------------------------------------------ */
/* Per-TTY backing-grid access                                          */
/* ------------------------------------------------------------------ */

/* Returns the backing buffer for slot n, or NULL if n is out of range
 * or vtty has not initialised buffers yet. */
vt_buf_t *vtty_buf(int n);

/* Returns the backing buffer bound to the calling task's TTY index, or
 * NULL if the task has TASK_TTY_NONE (e.g. idle, ktest_bg, boot CPU). */
vt_buf_t *vtty_buf_current(void);

/* Returns the backing buffer for the currently focused TTY, or NULL if
 * buffers are not yet allocated. */
vt_buf_t *vtty_buf_focused(void);

/* Apply any pending vtty_switch repaint deferred from IRQ context.
 * Safe to call from task context (yields, REPL polling); cheap when
 * no switch is pending. */
void vtty_drain_pending(void);

/* Request a deferred repaint of slot from task context (sets vtty_pending).
 * Used by SYS_WAIT4 to trigger a VT clear+repaint after a fullscreen child
 * exits, without doing pixel work in interrupt/syscall context. */
void vtty_request_repaint(int slot);

/* Foreground task override.  When a slot has a foreground task set
 * (e.g. shell_exec_elf's child taking focus), vtty_switch routes
 * keyboard focus + KEY_FOCUS_GAIN to that task instead of the slot's
 * registered shell.  Pass NULL to clear (reverts to slot owner).
 * No-op if slot is out of range. */
void vtty_set_foreground(int slot, task_t *t);

#endif /* _KERNEL_VTTY_H */
