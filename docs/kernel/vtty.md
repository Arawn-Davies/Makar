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
| `vtty_drain_pending()` | Drains a deferred switch repaint: if one is pending *and the calling task is on the destination TTY*, repaint that TTY's backing grid to the framebuffer and refresh the status bar (CAS'd so it fires exactly once per switch). Called from `keyboard_getchar`'s wait loop **and from the `SYS_YIELD` handler** — the latter so a fullscreen ring-3 app that never reads the keyboard in its draw loop (basic/lines) still flushes the repaint, letting the status bar follow a switch back to its VT. The grid is always repainted, restoring the destination VT's prior text + colour scheme and covering any raw pixels a fullscreen app left behind (display isolation); a continuously-drawing app repaints its own graphics next frame. |
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
bottom framebuffer row is reserved for the status bar
(`vesa_tty_paint_status` in vesa_tty.c). The multiplexer is named
**makmux** (the name it'll carry once lifted into a userspace daemon);
the bar shows `Makar  VT1 VT2 VT3 VT4  …  Alt+F1-F4` (1-based labels
matching the Alt+Fn keys) with the active slot highlighted; it is
repainted on
`vtty_register`, `vtty_drain_pending`, and `vesa_tty_clear` so it survives
focus changes and full-screen clears.

## Display isolation for ring-3 apps

The framebuffer-painting syscalls are gated on `vtty_is_focused()` so a
backgrounded fullscreen app can't bleed onto whatever VT is currently shown:

- `SYS_PUTCH_AT` / `SYS_SET_CURSOR` / `SYS_TTY_CLEAR` always record into the
  calling task's [vt_buf_t](vt.md) grid (so the compositor can repaint it on
  switch-back) but only touch the live framebuffer when focused.
- `SYS_DRAW_LINE` (raw pixels, no grid representation) is suppressed entirely
  when the task isn't on the active VT; the app redraws on its next frame.

`VTTY_MAX` is still 4, but the task pool (`MAX_TASKS`) is now 32, leaving room
for a fullscreen app on every VT plus headroom.
