# Split panes — historical WIP note (archived)

> **Status:** Phase 1 shipped. Phase 2/3 superseded — see below.

## What shipped

**Phase 1** — pane abstraction landed on main:

- `vesa_pane_t` (`top_row`, `cols`, `rows`, pane-relative cursor, fg/bg)
- `vesa_tty_pane_init`, `vesa_tty_pane_putchar`, `vesa_tty_pane_clear`,
  `vesa_tty_pane_setcolor`, `vesa_tty_pane_put_at`,
  `vesa_tty_pane_set_cursor`, `vesa_tty_pane_get_col/row`
- `vesa_tty_default_pane()` for legacy callers
- `pane_scroll_up` (in-pane memmove, no cross-pane interference)
- Used by VIX (`src/kernel/arch/i386/proc/vix.c`) to derive col/row
  counts at runtime.

## What replaced phases 2 & 3

The tmux-style `Ctrl-A` pane prefix was implemented inside the keyboard
driver (`keyboard.c`, search for `KB_PANE_TOP` / `KB_PANE_BOTTOM`), but
the wider "two shells side-by-side in one TTY" model was dropped in
favour of **independent TTYs**: `shell0`–`shell3` each get a full
screen, switched via `Alt+F1`–`Alt+F4`. The per-TTY backing buffer
(`vt_buf_t`, PR #129) makes Alt+Fn lossless; switching is functionally
better than a split because each TTY has its own kernel task, page
directory, and keyboard ring.

Pane API is still live — VIX uses it, future curses-style ELFs can
use it, but the shell itself is one-pane-per-TTY now.

See [`docs/kernel/shell.md`](../docs/kernel/shell.md) for the live
shell reference and [`docs/kernel/keyboard.md`](../docs/kernel/keyboard.md)
for the input routing (Alt+Fn / Ctrl-A handling).
