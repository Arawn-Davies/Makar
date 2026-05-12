#ifndef _KERNEL_SHELL_H
#define _KERNEL_SHELL_H

#include <stddef.h>

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

#endif /* _KERNEL_SHELL_H */
