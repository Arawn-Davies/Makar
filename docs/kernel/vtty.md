---
title: vtty
parent: Kernel subsystems
grand_parent: Reference
---

# vtty - virtual TTY manager

`src/kernel/include/kernel/vtty.h` + `src/kernel/arch/i386/proc/vtty.c`.

## Purpose

`VTTY_MAX = 5` slots exist in total:

- **Slots 0–3** are the user-visible VT shells (makmux's VT1–VT4, switched via
  Alt+F1–F4 or Ctrl+Tab / Ctrl+Shift+Tab).
- **Slot 4 (`VTTY_ROOT_SLOT`)** is the hidden root console for mak.sh0. It has
  a full backing buffer so mak.sh0's screen content is preserved across makmux
  sessions, but it is excluded from Alt+Fn switching, Ctrl+Tab cycling,
  `vtty_live_mask()`, and `vtty_count()` — exactly like Linux's tty0/console.

Only the active VT paints to the physical framebuffer; background VTs keep
writing into their own [vt_buf_t](vt.md) and the accumulated state is
repainted atomically when the user switches back. This is Linux's classic VT
model.

## Authoritative state

`task_t.tty` (in `kernel/task.h`) is authoritative for which TTY a task is
bound to. vtty itself owns:

| Field | Purpose |
|---|---|
| `vtty_nslots` | Number of registered user-visible slots (0–4; **excludes** `VTTY_ROOT_SLOT`) |
| `vtty_current` | Index of the focused user-visible slot |
| `vtty_bufs[VTTY_MAX]` | Per-slot [vt_buf_t](vt.md) backing grids (5 total) |
| `vtty_pending` | Deferred-paint target (set in IRQ, drained in task context) |

The "owning task" of slot N is resolved by walking the task pool for the
live task with the lowest pid whose `task->tty == N` (the registered shell —
exec children inherit `tty` but always have higher pids).

## Root slot semantics

mak.sh0 (the underlying ring-3 login shell spawned at boot) occupies
`VTTY_ROOT_SLOT = 4` via `vtty_register_root()`:

- `vtty_nslots` is **not** incremented, so the slot is invisible to
  `vtty_count()` and the Ctrl+Tab cycling range.
- `vtty_buf_focused()` returns `vtty_bufs[4]` when `vtty_nslots == 0`
  (no makmux VTs active), so mak.sh0's writes render to the framebuffer
  normally.
- `vtty_is_focused()` returns true for slot 4 whenever `vtty_nslots == 0`.
- `task_fork` strips `VTTY_ROOT_SLOT` from children — they inherit
  `TASK_TTY_NONE` instead. This prevents makmux and its sh.elf children from
  writing into `vtty_bufs[4]` and corrupting mak.sh0's preserved content.
- When all makmux VTs close (`vtty_nslots` → 0), `vtty_close_pid` calls
  `vesa_tty_set_status_visible(0)` to erase the stale status-bar row, then
  `SYS_WAIT4` requests a repaint of slot 4 — restoring mak.sh0's prior screen.

## API

| Function | Purpose |
|---|---|
| `vtty_init()` | Allocate five backing grids sized to the display geometry (last row reserved for the status bar). |
| `vtty_register()` | Called by each makmux child via `SYS_VT_ENTER`. Assigns the calling task to the next free slot (0–3) and increments `vtty_nslots`. |
| `vtty_register_root()` | Called by mak.sh0 at boot. Assigns `VTTY_ROOT_SLOT = 4` without touching `vtty_nslots`, making it permanently invisible to switching and the status bar. |
| `vtty_switch(n)` | Called from the keyboard IRQ when Alt+F<n+1> or Ctrl+Tab fires. Rejects `VTTY_ROOT_SLOT`. Updates focus, sends `KEY_FOCUS_GAIN` to the new owner, records a pending repaint target. Returns immediately — the paint is deferred. |
| `vtty_drain_pending()` | Drains a deferred switch repaint: if one is pending *and the calling task is on the destination TTY*, repaint that TTY's backing grid to the framebuffer (CAS'd so it fires exactly once per switch). Called from `keyboard_getchar`'s wait loop **and from the `SYS_YIELD` handler** so fullscreen ring-3 apps that never read the keyboard still flush the repaint. |
| `vtty_close_pid(pid)` | Unregisters a VT child. Rejects `VTTY_ROOT_SLOT`. When `vtty_nslots` drops to 0, clears the status bar row. |
| `vtty_buf(n)` / `vtty_buf_current()` / `vtty_buf_focused()` | Accessors for renderer code. `vtty_buf_focused()` returns `vtty_bufs[4]` when `vtty_nslots == 0`. |
| `vtty_active()` / `vtty_count()` / `vtty_is_focused()` / `vtty_live_mask()` | State queries. All exclude `VTTY_ROOT_SLOT`. |
| `vtty_request_repaint(slot)` | Deferred repaint trigger used by `SYS_WAIT4` after a fullscreen child exits. |

## Why deferred paint

Painting the framebuffer from `vtty_switch` directly meant doing thousands
of pixel writes inside the keyboard IRQ handler. That held the i8042's
1-byte OBF full long enough that the edge-triggered PIC missed subsequent
key edges — the same failure shape as PR #127's regression. Recording a
pending target and letting `keyboard_getchar` drain it in task context
keeps the IRQ short.

## Status bar

The status bar is drawn entirely in **userspace** by `makmux.elf` via
`SYS_PUTCH_AT`. The kernel only reserves the bottom framebuffer row
(`VESA_TTY_STATUS_ROWS = 1`) and exposes `vesa_tty_paint_cell` so cells at
`row >= pane_rows` bypass the default-pane clipping and reach the status row.

The bar shows tab indicators `VT1 VT2 VT3 VT4` on an **amber/brown**
background (`VGA_BROWN = 0xAA5500`), with the active slot highlighted in
yellow-on-black. Alt+F5 toggles a live RTC clock in the label area.

**Switching shortcuts:**

| Shortcut | Action |
|---|---|
| Alt+F1–F4 | Jump to VT 1–4 directly |
| Ctrl+Tab | Cycle to the next VT |
| Ctrl+Shift+Tab | Cycle to the previous VT |

Ctrl+Tab / Ctrl+Shift+Tab were added for Windows hosts where Alt+F4 is
captured by the OS.

## Display isolation for ring-3 apps

The framebuffer-painting syscalls are gated on `vtty_is_focused()` so a
backgrounded fullscreen app can't bleed onto whatever VT is currently shown:

- `SYS_PUTCH_AT` / `SYS_SET_CURSOR` / `SYS_TTY_CLEAR` always record into the
  calling task's [vt_buf_t](vt.md) grid (so the compositor can repaint it on
  switch-back) but only touch the live framebuffer when focused.
- `SYS_DRAW_LINE` (raw pixels, no grid representation) is suppressed entirely
  when the task isn't on the active VT; the app redraws on its next frame.

`VTTY_MAX = 5` (four user-visible slots + one hidden root console), and the
task pool (`MAX_TASKS`) is 32, leaving room for a fullscreen app on every VT
plus headroom.
