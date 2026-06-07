#ifndef _KERNEL_KEYBOARD_H
#define _KERNEL_KEYBOARD_H

#include <stdint.h>
#include <kernel/task.h>

/* KEY_* input sentinels: the canonical list is shared with userspace. */
#include <makar_keys.h>

/* Pane IDs for keyboard_bind_pane() / keyboard_focus_pane(). */
#define KB_PANE_TOP     0
#define KB_PANE_BOTTOM  1

void keyboard_init(void);
unsigned char keyboard_getchar(void);
unsigned char keyboard_poll(void);

/* Atomically test-and-clear the pending Ctrl-Alt-Del flag (1 if it was set).
 * Backs SYS_CAD_PENDING so the GUI server can open its power menu instantly. */
int kb_take_cad_pending(void);

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

/* keyboard_set_scancode - enable raw set-1 scancode passthrough (make+break,
 * 0x80=break) for full-keyboard apps (games) that need key-release events.
 * Mutually exclusive with cooked/raw sentinel delivery. */
void keyboard_set_scancode(int on);

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

/* Synthetic key injection for the in-guest keyboard test harness (drives the
 * live shell, not the isolated test ring).  keyboard_inject_key presses one
 * key with optional modifiers; keyboard_inject_text types a string.
 * keyboard_test_driver runs the scripted scenarios (spawned when `kbtest` is
 * on the cmdline). */
void keyboard_inject_key(uint8_t kc, int shift, int ctrl, int alt);
void keyboard_inject_text(const char *s);
void keyboard_test_driver(void);

#endif /* _KERNEL_KEYBOARD_H */
