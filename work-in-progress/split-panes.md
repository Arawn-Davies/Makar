# Split panes - tmux-style VESA terminal splitting

> **Status:** Phase 1 complete; Phases 2–3 in progress
> **Branch:** `feat/split-panes`

## Summary

Horizontal split-pane support in the VESA framebuffer renderer.
Each pane owns its cursor, colours, and a row sub-rectangle of the screen.
VGA text mode falls back to virtual-console switching via the same Ctrl-A prefix.

## Phase 1 - Pane abstraction (complete)

- [x] `vesa_pane_t` struct: `top_row`, `cols`, `rows`, pane-relative cursor, fg/bg
- [x] `vesa_tty_pane_init`, `vesa_tty_pane_putchar`, `vesa_tty_pane_clear`,
      `vesa_tty_pane_setcolor`, `vesa_tty_pane_put_at`, `vesa_tty_pane_set_cursor`,
      `vesa_tty_pane_get_col`, `vesa_tty_pane_get_row`
- [x] `vesa_tty_default_pane()` - legacy callers unaffected (screen-spanning pane)
- [x] `pane_scroll_up` - in-pane memmove, does not disturb other panes
- [x] Full ktest + GDB boot suite passes

## Phase 2 - Keyboard dispatcher (TODO)

- [ ] Ctrl-A prefix dispatcher: Ctrl-A,U / Ctrl-A,J switch top/bottom pane focus
- [ ] VGA/low-res fallback: Ctrl-A,1/2/3 switch full-screen virtual consoles
- [ ] Per-task input queues; `keyboard_getchar()` dequeues from the calling
      task's bound queue
- [ ] Focus pointer; Ctrl-A prefix consumed by dispatcher, not forwarded

## Phase 3 - VICS pane integration (TODO)

- [ ] Refactor `vics.c` to accept `(top_row, text_rows, status_row)` instead
      of hard-coding rows 0–23 + 24 + raw `VGA_MEMORY` writes
- [ ] `splitscreen` shell command: spawn VICS in top pane, shell in bottom
- [ ] Mode detection: `vesa_tty_is_ready() && vesa_tty_get_rows() >= 30`

## Constraints

- Horizontal splits only (top/bottom); side-by-side column splits are out of scope
- Split mode is VESA-only; VGA text mode uses virtual-console switching
