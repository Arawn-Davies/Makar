---
title: vtty
parent: Kernel subsystems
---

# vtty - virtual TTY manager

`src/kernel/include/kernel/vtty.h` + `src/kernel/arch/i386/proc/vtty.c`.

## Purpose

Up to `VTTY_MAX = 4` shell tasks run concurrently. Alt+F1–F4 switches the
active (focused) TTY. Only the active TTY paints to the physical framebuffer;
background TTYs keep writing into their own [vt_buf_t](vt.md) and the
accumulated state is repainted atomically when the user switches back. This
is Linux's classic VT model.

## Authoritative state

`task_t.tty` (in `kernel/task.h`) is authoritative for which TTY a task is
bound to. vtty itself owns:

| Field | Purpose |
|---|---|
| `vtty_nslots` | How many slots have been registered |
| `vtty_current` | Index of the focused slot |
| `vtty_bufs[VTTY_MAX]` | Per-slot [vt_buf_t](vt.md) backing grids |
| `vtty_pending` | Deferred-paint target (set in IRQ, drained in task context) |

The "owning task" of slot N is resolved by walking the task pool for the
live task with the lowest pid whose `task->tty == N` (the registered shell —
exec children inherit `tty` but always have higher pids).

## API

| Function | Purpose |
|---|---|
| `vtty_init()` | Allocate the four backing grids sized to the display geometry (last row reserved for the status bar). |
| `vtty_register()` | Called by `shell_run` at start. Assigns the calling task to the next free slot and sets `task->tty`. |
| `vtty_switch(n)` | Called from the keyboard IRQ when Alt+F<n+1> fires. Updates focus, sends `KEY_FOCUS_GAIN` to the new owner, records a pending repaint target. Returns immediately - the paint is deferred. |
| `vtty_drain_pending()` | Called from `keyboard_getchar`'s wait loop. If a pending paint exists *and the calling task owns the destination TTY*, paint the new buffer to the framebuffer. The owner-check avoids races where a non-focused shell would paint over the destination shell's concurrent writes. |
| `vtty_buf(n)` / `vtty_buf_current()` / `vtty_buf_focused()` | Accessors for renderer code. |
| `vtty_active()` / `vtty_count()` / `vtty_is_focused()` | State queries. |

## Why deferred paint

Painting the framebuffer from `vtty_switch` directly meant doing thousands
of pixel writes inside the keyboard IRQ handler. That held the i8042's
1-byte OBF full long enough that the edge-triggered PIC missed subsequent
key edges - the same failure shape as PR #127's regression. Recording a
pending target and letting `keyboard_getchar` drain it in task context
keeps the IRQ short.

## Status bar

`vtty_init` shrinks each backing grid by `VTTY_STATUS_ROWS = 1` so the
bottom framebuffer row is reserved for a tmux-style indicator
(`vesa_tty_paint_status` in vesa_tty.c). The bar lists `VT0 VT1 VT2 VT3`
with the active slot highlighted; it is repainted on `vtty_register`,
`vtty_switch`, and `vesa_tty_clear` so it survives focus changes and
full-screen clears.
