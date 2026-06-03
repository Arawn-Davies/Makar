#ifndef _KERNEL_SHELL_H
#define _KERNEL_SHELL_H

#include <stddef.h>

/* Set to 1 by kernel_main when `live` appears on the kernel cmdline.
 * shell_login_loop reads this to skip authentication on live ISO sessions. */
extern int g_live_boot;

/* `autologin=<user>` from the kernel cmdline (empty = unset).  Passed to
 * auth_try_autologin by shell_login_loop; overrides /etc/autologin. */
extern char g_autologin_user[64];

/*
 * Minimal kernel REPL over VGA + PS/2 keyboard.
 *
 * shell_run() must be called after all subsystems (keyboard, heap, timer,
 * VGA) have been initialised.  It never returns.
 */
void shell_run(void);

/*
 * shell_readline – read and echo one line from the PS/2 keyboard into buf.
 *
 * Supports inline cursor movement (←/→), backspace at cursor, and history
 * navigation (↑/↓).  Terminates on Enter; always NUL-terminates buf.
 * Reads at most (max - 1) characters.
 *
 * Exposed here so the syscall layer can provide echoing stdin reads to
 * ring-3 user-space processes (SYS_READ fd=0).
 */
void shell_readline(char *buf, size_t max);

/*
 * shell_clear_screen – the exact code path the `clear` shell command runs.
 *
 * Resets the VGA colour scheme to SHELL_COLOR_VGA, sets the VESA pane fg/bg
 * to SHELL_FG_RGB/SHELL_BG_RGB, fills the framebuffer with the new bg, and
 * resets the cursors.  Exposed so SYS_SHELL_CLEAR / ring-3 apps can reach
 * an identical clean canvas without re-implementing colour reset.
 */
void shell_clear_screen(void);

/*
 * shell_apply_scheme_for_tty – apply slot `tty`'s palette to the active
 * renderer (VGA attribute byte + VESA fg/bg).  Exposed so the boot
 * userspace-shell task can paint the right colours before exec'ing
 * /apps/sh.elf; out-of-range tty falls back to slot 0.
 */
void shell_apply_scheme_for_tty(int tty);

/*
 * shell_enter_slot – the full per-VT shell prelude.  Sets SIGINT ignore,
 * marks the task unkillable, registers the VT slot, runs the loading
 * screen (slot 0 only) and waits for ktest_bg_done, waits for focus
 * (non-primary slots), applies the per-VT palette, and clears the
 * screen on first focus.
 *
 * Returns the assigned slot index (>= 0), or -1 if vtty_register failed
 * (caller should bail).  Called once per shell task; both shell_run
 * (in-kernel rescue path) and user_shell_slot_entry (userspace login
 * path) go through this so the UX is identical.
 */
int shell_enter_slot(int with_loading_screen);

/*
 * shell_enter_makmux_slot – attach the current task to a makmux-owned VT
 * and name it mak.sh<N>.  focus_new selects whether the new pane becomes
 * active immediately (Alt-T replacement behavior).  Unlike shell_enter_slot(),
 * this is specifically for userspace makmux children, so it re-enables the
 * kernel status bar hidden by mak.sh0 normal boot and preserves the original
 * VT palettes.
 */
int shell_enter_makmux_slot(int focus_new);

/*
 * shell_enter_root_tty – boot prelude for the detached mak.sh0 login shell.
 * This does not register a vtty slot; makmux owns the four switchable VT
 * slots and creates mak.sh1..mak.sh4 as its children.
 */
void shell_enter_root_tty(void);

/*
 * shell_login_loop – mak.sh0 session manager.
 *
 * Called after shell_enter_root_tty() completes.  Loops forever:
 *   1. Show login_screen() (blocks until authenticated).
 *   2. Spawn /apps/sh.elf --login as a child task and wait for it to exit.
 *   3. Go to 1.
 *
 * Typing `exit` or Ctrl-D in mak.sh0 returns here; mak.sh1-4 (spawned by
 * makmux) never call this and exit normally without re-authentication.
 */
void shell_login_loop(void);

#endif /* _KERNEL_SHELL_H */
