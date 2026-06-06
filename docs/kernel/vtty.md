---
title: vtty
parent: Kernel subsystems
grand_parent: Reference
---

# vtty - virtual TTY manager

`src/kernel/include/kernel/vtty.h` + `src/kernel/arch/i386/proc/vtty.c`.

## Purpose

`VTTY_MAX = 10` slots exist in total:

- **Slots 0–8** are the nine user-visible VT shells (makmux's VT1–VT9). They
  map 1:1 to the userspace numbering exposed under `/dev`: **slot `i` ==
  `/dev/tty(i+1)`**. Alt+F1–F4 jump directly to slots 0–3; the rest are reached
  by Alt+Tab / Ctrl+Tab (forward) and Alt+Shift+Tab / Ctrl+Shift+Tab (back).
- **Slot 9 (`VTTY_ROOT_SLOT`)** is the hidden root console for mak.sh0, exposed
  as `/dev/tty0`. It has a full backing buffer so mak.sh0's screen content is
  preserved across makmux sessions, but it is excluded from Alt+Fn switching,
  Ctrl+Tab cycling, `vtty_live_mask()`, and `vtty_count()` — exactly like
  Linux's tty0/console.

makmux drives these slots tmux-style: **one** shell exists by default and more
are created on demand via Alt+T (`SYS_VT_OPEN_REQUEST`), up to nine.

Only the active VT paints to the physical framebuffer; background VTs keep
writing into their own [vt_buf_t](vt.md) and the accumulated state is
repainted atomically when the user switches back. This is Linux's classic VT
model.

## /dev/ttyN nodes

Every slot is addressable by a stable Linux-style path: `devfs` registers a
`DEV_TTY` node for `/dev/tty0` (root console) and `/dev/tty1`..`/dev/tty9` (the
nine user slots), always present regardless of makmux state. They are character
sinks, not block devices — reads return EOF, and a write streams into the
slot's backing grid via `vtty_write()` (painting the framebuffer when that VT
is focused), so `echo hi > /dev/tty2` lands on VT2. `devfs_node_location`
rejects them, so they can't be mounted.

## Authoritative state

`task_t.tty` (in `kernel/task.h`) is authoritative for which TTY a task is
bound to. vtty itself owns:

| Field | Purpose |
|---|---|
| `vtty_nslots` | High-water number of registered user-visible slots (0–9; **excludes** `VTTY_ROOT_SLOT`) |
| `vtty_current` | Index of the focused user-visible slot |
| `vtty_bufs[VTTY_MAX]` | Per-slot [vt_buf_t](vt.md) backing grids (10 total) |
| `vtty_pending` | Deferred-paint target (set in IRQ, drained in task context) |

The "owning task" of slot N is resolved by walking the task pool for the
live task with the lowest pid whose `task->tty == N` (the registered shell —
exec children inherit `tty` but always have higher pids).

## Root slot semantics

mak.sh0 (the underlying ring-3 login shell spawned at boot) occupies
`VTTY_ROOT_SLOT = 9` (exposed as `/dev/tty0`) via `vtty_register_root()`:

- `vtty_nslots` is **not** incremented, so the slot is invisible to
  `vtty_count()` and the Ctrl+Tab cycling range.
- `vtty_buf_focused()` returns `vtty_bufs[VTTY_ROOT_SLOT]` when
  `vtty_nslots == 0` (no makmux VTs active), so mak.sh0's writes render to the
  framebuffer normally.
- `vtty_is_focused()` returns true for the root slot whenever `vtty_nslots == 0`.
- `task_fork` strips `VTTY_ROOT_SLOT` from children — they inherit
  `TASK_TTY_NONE` instead. This prevents makmux and its sh.elf children from
  writing into the root slot's grid and corrupting mak.sh0's preserved content.
- When all makmux VTs close (`vtty_nslots` → 0), `vtty_close_pid` calls
  `vesa_tty_set_status_visible(0)` to erase the stale status-bar row, then
  `SYS_WAIT4` requests a repaint of the root slot — restoring mak.sh0's screen.

## API

| Function | Purpose |
|---|---|
| `vtty_init()` | Allocate ten backing grids sized to the display geometry (last row reserved for the status bar). |
| `vtty_register()` | Called by each makmux child via `SYS_VT_ENTER`. Assigns the calling task to the next free slot (0–8) and raises the `vtty_nslots` high-water mark. |
| `vtty_register_root()` | Called by mak.sh0 at boot. Assigns `VTTY_ROOT_SLOT = 9` without touching `vtty_nslots`, making it permanently invisible to switching and the status bar. |
| `vtty_switch(n)` | Called from the keyboard IRQ when Alt+F<n+1> or Ctrl+Tab fires. Rejects `VTTY_ROOT_SLOT`. Updates focus, sends `KEY_FOCUS_GAIN` to the new owner, records a pending repaint target. Returns immediately — the paint is deferred. |
| `vtty_write(n, buf, len)` | Stream bytes into slot `n`'s backing grid (the `/dev/ttyN` write sink); repaints the framebuffer if `n` is focused. |
| `vtty_drain_pending()` | Drains a deferred switch repaint: if one is pending *and the calling task is on the destination TTY*, repaint that TTY's backing grid to the framebuffer (CAS'd so it fires exactly once per switch). Called from `keyboard_getchar`'s wait loop **and from the `SYS_YIELD` handler** so fullscreen ring-3 apps that never read the keyboard still flush the repaint. |
| `vtty_close_pid(pid)` | Unregisters a VT child. Rejects `VTTY_ROOT_SLOT`. When `vtty_nslots` drops to 0, clears the status bar row. |
| `vtty_buf(n)` / `vtty_buf_current()` / `vtty_buf_focused()` | Accessors for renderer code. `vtty_buf_focused()` returns the root slot's grid when `vtty_nslots == 0`. |
| `vtty_active()` / `vtty_count()` / `vtty_is_focused()` / `vtty_live_mask()` | State queries. All exclude `VTTY_ROOT_SLOT`. |
| `vtty_request_repaint(slot)` | Deferred repaint trigger used by `SYS_WAIT4` after a fullscreen child exits. |

## Why deferred paint

Painting the framebuffer from `vtty_switch` directly meant doing thousands
of pixel writes inside the keyboard IRQ handler. That held the i8042's
1-byte OBF full long enough that the edge-triggered PIC could miss subsequent
key edges. Recording a pending target and letting `keyboard_getchar` drain it
in task context keeps the IRQ short.

## Status bar

The status bar is drawn entirely in **userspace** by `statusbar.elf` via
`SYS_PUTCH_AT`. The kernel only reserves the bottom framebuffer row
(`VESA_TTY_STATUS_ROWS = 1`) and exposes `vesa_tty_paint_cell` so cells at
`row >= pane_rows` bypass the default-pane clipping and reach the status row.

The bar shows tab indicators `VT1`..`VT9` on an **amber/brown** background
(`VGA_BROWN = 0xAA5500`), with the active slot highlighted in yellow-on-black.

The bar is **per-tab**: `statusbar.elf` renders the layout belonging to the
active VT, read from `SYS_VT_STATE`. Each tab can own its layout via
`~/.sbrc.tty<N>` (the `/dev/ttyN` number; root console = `tty0`), which
overrides the shared `~/.sbrc`; switching VTs reloads and re-renders that tab's
own bar. See [GUI](../gui.md) for the `.sbrc` widget format.

**Switching shortcuts:**

| Shortcut | Action |
|---|---|
| Alt+F1–F4 | Jump to VT 1–4 directly |
| Alt+T | Open a new tab (tmux-style; up to nine) |
| Alt+Tab / Ctrl+Tab | Cycle to the next VT |
| Alt+Shift+Tab / Ctrl+Shift+Tab | Cycle to the previous VT |

Ctrl+Tab / Ctrl+Shift+Tab were added for Windows hosts where Alt+Tab/Alt+F4 is
captured by the OS.

## Display isolation for ring-3 apps

The framebuffer-painting syscalls are gated on `vtty_is_focused()` so a
backgrounded fullscreen app can't bleed onto whatever VT is currently shown:

- `SYS_PUTCH_AT` / `SYS_SET_CURSOR` / `SYS_TTY_CLEAR` always record into the
  calling task's [vt_buf_t](vt.md) grid (so the compositor can repaint it on
  switch-back) but only touch the live framebuffer when focused.
- `SYS_DRAW_LINE` (raw pixels, no grid representation) is suppressed entirely
  when the task isn't on the active VT; the app redraws on its next frame.

`VTTY_MAX = 10` (nine user-visible slots + one hidden root console), and the
task pool (`MAX_TASKS`) is 32, leaving room for a fullscreen app on the active
VTs plus headroom.
