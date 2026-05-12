#ifndef _KERNEL_KEYBOARD_H
#define _KERNEL_KEYBOARD_H

#include <stdint.h>
#include <kernel/task.h>

/*
 * Sentinels for extended (non-ASCII) keys - high-byte range so they never
 * collide with any Ctrl+letter code (0x01-0x1A).
 */
#define KEY_ARROW_UP    ((char)0x80)
#define KEY_ARROW_DOWN  ((char)0x81)
#define KEY_ARROW_LEFT  ((char)0x82)
#define KEY_ARROW_RIGHT ((char)0x83)

/* Function key sentinels (Alt+F1-F4 trigger TTY switching).
 * F1..F12 occupy 0x84..0x87 (legacy F1-F4) and 0x89..0x90 (F5-F12). */
#define KEY_F1          ((char)0x84)
#define KEY_F2          ((char)0x85)
#define KEY_F3          ((char)0x86)
#define KEY_F4          ((char)0x87)

/* Sent to a TTY's input queue when it gains keyboard focus. */
#define KEY_FOCUS_GAIN  ((char)0x88)

#define KEY_F5          ((char)0x89)
#define KEY_F6          ((char)0x8A)
#define KEY_F7          ((char)0x8B)
#define KEY_F8          ((char)0x8C)
#define KEY_F9          ((char)0x8D)
#define KEY_F10         ((char)0x8E)
#define KEY_F11         ((char)0x8F)
#define KEY_F12         ((char)0x90)

/* Modifier-press sentinels.  Emitted on the make event for the modifier
 * itself (release is silent).  Lets diagnostic tools like kbtester light
 * up the cell when the user presses Shift/Ctrl/Alt/Caps alone.  Shells
 * silently drop them (anything < 0x20 except handled sentinels falls
 * through the `c < 0x20 || c > 0x7E` printable filter). */
#define KEY_SHIFT_DOWN  ((char)0x91)
#define KEY_CTRL_DOWN   ((char)0x92)
#define KEY_ALT_DOWN    ((char)0x93)
#define KEY_CAPS_TOGGLE ((char)0x94)
#define KEY_SUPER_DOWN  ((char)0x95)
#define KEY_MENU_DOWN   ((char)0x96)

/* Ctrl+C sentinel returned by keyboard_getchar() when a sigint fires. */
#define KEY_CTRL_C      ((char)0x03)

/* Pane IDs for keyboard_bind_pane() / keyboard_focus_pane(). */
#define KB_PANE_TOP     0
#define KB_PANE_BOTTOM  1

void keyboard_init(void);
char keyboard_getchar(void);
char keyboard_poll(void);

/* Per-task input routing (Phase 2 / split-panes). */
void keyboard_bind_pane(int pane_id, task_t *t);
void keyboard_focus_pane(int pane_id);
void keyboard_set_focus(task_t *t);

/* Release a task's keyboard slot so it can be reused. */
void keyboard_release_task(task_t *t);

/* Push c directly into t's input slot (registers slot if needed).
 * Safe to call from IRQ context. */
void keyboard_send_to(task_t *t, char c);

/*
 * keyboard_sigint_consume – atomically read and clear the Ctrl+C flag.
 * Returns 1 if Ctrl+C was pressed since the last call, 0 otherwise.
 * Used by cmd_exec to force-kill a running user task.
 */
int keyboard_sigint_consume(void);

/*
 * keyboard_set_raw – enable/disable raw key event delivery.
 *
 * In raw mode the cooked-mode shortcuts that normally swallow keys are
 * suspended for the duration of the call:
 *   - Alt+F1..F4 stop switching virtual TTYs
 *   - Ctrl+A no longer arms the pane-switch prefix
 *   - Modifier presses (Shift/Ctrl/Alt/Caps/Super/Menu) deliver KEY_*_DOWN
 *     sentinels instead of being silent
 *   - F1..F12 always deliver KEY_F1..KEY_F12 sentinels
 *
 * Ctrl+C still routes 0x03 and sets the sigint flag - so a raw-mode app
 * can still be exited the usual way.
 *
 * Diagnostic tools like kbtester call this on entry and pair the disable
 * with their cleanup path so the next focused task gets the cooked
 * behaviour back.
 */
void keyboard_set_raw(int on);

#endif /* _KERNEL_KEYBOARD_H */
