#ifndef _KERNEL_KEYBOARD_H
#define _KERNEL_KEYBOARD_H

#include <stdint.h>
#include <kernel/task.h>

/*
 * Sentinels for extended (non-ASCII) keys - high-byte range so they never
 * collide with any Ctrl+letter code (0x01-0x1A).
 *
 * Cast as `unsigned char` (not plain `char`) so the type stays positive
 * through integer promotion. A previous `((char)0x80)` form silently broke
 * any consumer that read into `unsigned char c` -- the comparison
 * `c == KEY_ARROW_RIGHT` widened c to int 0x83 and the macro to int -125,
 * never matched, and at high optimisation jump-table dispatch could land on
 * a wild address (EIP=0xFFFFFF83 observed on PR #124 / kbtester).
 */
#define KEY_ARROW_UP    ((unsigned char)0x80)
#define KEY_ARROW_DOWN  ((unsigned char)0x81)
#define KEY_ARROW_LEFT  ((unsigned char)0x82)
#define KEY_ARROW_RIGHT ((unsigned char)0x83)

/* Function key sentinels (Alt+F1-F4 trigger TTY switching).
 * F1..F12 occupy 0x84..0x87 (legacy F1-F4) and 0x89..0x90 (F5-F12). */
#define KEY_F1          ((unsigned char)0x84)
#define KEY_F2          ((unsigned char)0x85)
#define KEY_F3          ((unsigned char)0x86)
#define KEY_F4          ((unsigned char)0x87)

/* Sent to a TTY's input queue when it gains keyboard focus. */
#define KEY_FOCUS_GAIN  ((unsigned char)0x88)

#define KEY_F5          ((unsigned char)0x89)
#define KEY_F6          ((unsigned char)0x8A)
#define KEY_F7          ((unsigned char)0x8B)
#define KEY_F8          ((unsigned char)0x8C)
#define KEY_F9          ((unsigned char)0x8D)
#define KEY_F10         ((unsigned char)0x8E)
#define KEY_F11         ((unsigned char)0x8F)
#define KEY_F12         ((unsigned char)0x90)

/* Modifier-press sentinels.  Emitted on the make event for the modifier
 * itself (release is silent).  Lets diagnostic tools like kbtester light
 * up the cell when the user presses Shift/Ctrl/Alt/Caps alone.  Shells
 * silently drop them (anything < 0x20 except handled sentinels falls
 * through the `c < 0x20 || c > 0x7E` printable filter). */
#define KEY_SHIFT_DOWN  ((unsigned char)0x91)
#define KEY_CTRL_DOWN   ((unsigned char)0x92)
#define KEY_ALT_DOWN    ((unsigned char)0x93)
#define KEY_CAPS_TOGGLE ((unsigned char)0x94)
#define KEY_SUPER_DOWN  ((unsigned char)0x95)
#define KEY_MENU_DOWN   ((unsigned char)0x96)

/* Ctrl+C sentinel returned by keyboard_getchar() when a sigint fires. */
#define KEY_CTRL_C      ((unsigned char)0x03)

/* Pane IDs for keyboard_bind_pane() / keyboard_focus_pane(). */
#define KB_PANE_TOP     0
#define KB_PANE_BOTTOM  1

void keyboard_init(void);
unsigned char keyboard_getchar(void);
unsigned char keyboard_poll(void);

/* Per-task input routing (Phase 2 / split-panes). */
void keyboard_bind_pane(int pane_id, task_t *t);
void keyboard_focus_pane(int pane_id);
void keyboard_set_focus(task_t *t);

/* Release a task's keyboard slot so it can be reused. */
void keyboard_release_task(task_t *t);

/* Push c directly into t's input slot (registers slot if needed).
 * Safe to call from IRQ context. */
void keyboard_send_to(task_t *t, unsigned char c);

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

/* ===========================================================================
 * Test hooks (in-kernel ktest harness).
 *
 * Drive the decoder synchronously from kernel context and read back what it
 * would have routed.  keyboard_test_begin() saves and clears the focused task
 * so routed bytes land in the global fallback ring where keyboard_test_drain()
 * can read them deterministically; keyboard_test_end() restores focus.
 *
 * keyboard_test_mod_state() returns a packed snapshot of the modifier flags:
 *   bit 0  mod_shift   bit 1  mod_ctrl   bit 2  mod_alt    bit 3  mod_caps
 *   bit 4  mod_lshift  bit 5  mod_rshift bit 6  mod_lctrl  bit 7  mod_rctrl
 *   bit 8  mod_lalt    bit 9  mod_ralt
 * ======================================================================== */

void     keyboard_test_begin(void);
void     keyboard_test_end(void);
void     keyboard_test_feed(uint8_t sc);
int      keyboard_test_drain(unsigned char *out);
void     keyboard_test_reset(void);
uint32_t keyboard_test_mod_state(void);

/* keyboard_test_leds - returns the most recent LED bitmap kb_sync_leds
 * computed (bit 0 Scroll, bit 1 Num, bit 2 Caps -- same wire format as
 * the 0xED data byte).  keyboard_test_led_sends counts how many times
 * kb_sync_leds was invoked since boot, so a test can assert that a Caps
 * press fired exactly one LED update. */
uint8_t  keyboard_test_leds(void);
uint32_t keyboard_test_led_sends(void);

#endif /* _KERNEL_KEYBOARD_H */
