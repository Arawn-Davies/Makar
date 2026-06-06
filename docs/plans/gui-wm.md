# GUI window manager — design & slice plan

Status: **PoC implemented** (branch `feature/gui_planning`). A double-buffered,
mouse-driven desktop runs; this doc keeps the architecture + the remaining queue.

## Implemented (this PR)

- **PS/2 mouse driver** (`drivers/mouse.c`, IRQ12, SPSC event ring,
  `mouse_inject_packet` test hook; keyboard IRQ1 reroutes AUX bytes to it).
- **Syscalls** `SYS_MOUSE_READ` (256), `SYS_FB_PRESENT` (257, full-frame blit).
- **Scancode passthrough** (`SYS_KEYBOARD_RAW(2)`): raw set-1 make+break stream
  for full-keyboard apps (the keyboard previously only delivered makes).
- **`gui.elf`** (type `gui`): double-buffered compositor, draggable terminal
  window (xterm/xfce-style: dark bg, tango palette, block cursor, prompt),
  software cursor, bottom **dock** with live stat pills (mem/tasks/uptime/clock),
  desktop **icons** (Terminal/Files/Editor/Doom), `gui test` four-corner smoke
  test (serial `GUI-MOUSE-TEST: PASS`).
  - Ctrl-C closes the focused window (WM ignores SIGINT, never dies to ^C);
    ESC quits. Status bar forced off in gui, prior shell state restored on exit.
  - Editor/Doom icons `execve` the fullscreen app (`vix.elf`/`doom.elf`), handing
    kb+m focus over; control returns to the shell on exit.
- **USB** controller **detection** (`drivers/usb/usb.c`); input stays PS/2.
- **Doom** (`doom.elf`) via vendored doomgeneric — compiles + links; see #34.

## Deferred (next slices, in priority order)

1. **Alt+F5 / Alt+F6 VT switch** — make the gui a real VT: Alt+F6 → gui regains
   kb+mouse+framebuffer, Alt+F5 → shell regains them. Needs `vtty` integration +
   focus/fb-ownership handoff. NOTE: Alt+F5 is currently the Makar↔clock toggle
   (`keyboard.c on_make`); that binding must move/coexist.
2. **Live shell in the terminal window** — host real `sh.elf` via fork/exec with
   **non-blocking pipe** reads (kernel pipe read currently blocks).
3. **Windowed** file browser + text editor.  Basic fullscreen PoCs exist: the
   Files icon launches `files.elf` (list + arrow-nav + enter/backspace), the
   Editor icon reuses fullscreen `vix.elf`.  Making them in-window apps needs the
   windowed-hosting infra (item 2).
4. **In-OS tcc build of doom** (ship doomgeneric source + build script, mind the
   tcc-bundled stdint/limits headers).

## Goal

A Cosmos-style desktop on top of the existing 720p Bochs VBE framebuffer:

- A **double-buffered compositor** — draw a frame into a back buffer, present it.
- **Movable / openable / closable windows** with title bars, driven by a mouse.
- A **terminal window** hosting the *real* ring-3 userland shell (`sh.elf`, the
  same `mak.sh0-4` login shell that shows the current user).
- The terminal window borrows **makmux's tab concept** but does *not* run makmux:
  each terminal-window instance owns its own set of tabs, each tab a separate
  shell process.
- A **desktop** with a terminal **icon** that opens a new terminal window.
- A **status bar copied to the bottom**, a pixel bar mirroring the makmux info
  (user, clock) — drawn by the WM, not the cell-based makmux bar.

## Confirmed decisions

| Decision | Choice |
|---|---|
| WM execution model | **Userspace** ring-3 process (`wm.elf`) |
| Launch | Type **`gui`** at the prompt; on exit, restore the text console |
| Shell in window | **Reuse `sh.elf`** (the real login shell), one process per tab |
| Tabs | Concept borrowed from makmux; **per-window** tab set, not the makmux process |
| Pointer device | **PS/2 mouse only** (new kernel driver) |
| Compositing | Double-buffered: WM composites a back buffer, presents via one syscall |

## What exists today (foundation)

- `vesa_get_fb()` → `vesa_fb_t { addr, pitch, width, height, bpp(32), rgb shifts }`
  — direct framebuffer access. `src/kernel/include/kernel/vesa.h`.
- Fonts: `FONT8x8[128][8]`, `FONT8x16[256][16]` — for glyph rendering.
- Graphics syscalls: **`SYS_FB_INFO` (216)** returns `(w<<16)|h` only;
  **`SYS_DRAW_LINE` (217)** one clipped Bresenham line. That's the entire GPU ABI.
- Host primitives for running a shell in a window already exist:
  `SYS_FORK(2)`, `SYS_EXECVE(11)`, `SYS_PIPE(42)`, `SYS_DUP(41)`, `SYS_DUP2(63)`,
  `SYS_WAIT4(114)`, per-task fd table.
- The PS/2 driver (`drivers/keyboard.c`) reads the 8042 controller and currently
  **discards AUX-channel (mouse) bytes** (`PS2_STAT_AUXB`); aux device + IRQ12 are
  never enabled.

## What does NOT carry over

The existing on-screen text stack — `vesa_tty` panes, `vtty` virtual terminals,
the `makmux` userspace tab bar — is **character-cell** rendering. The WM is a
**pixel** layer beside it, not an extension of it. The "status bar copy" is drawn
fresh by the compositor in pixels; it mirrors makmux's *information*, not its code.

## New kernel ABI required

Kept deliberately small. The WM owns its own back buffer in anon memory and the
kernel only does the final copy-to-scanout, which avoids exposing the raw LFB
mapping and keeps the flip atomic.

1. **`SYS_FB_INFO2`** (or extend `FB_INFO`) — report `pitch` and `bpp` too, not
   just `w/h`. The WM needs pitch to size its back buffer.
2. **`SYS_FB_PRESENT(buf, x, y, w, h)`** — blit a `w×h` rect of the WM's 32-bpp
   back buffer to the framebuffer at `(x,y)`. Full-frame present first; dirty-rect
   args let us optimise later without an ABI change.
3. **PS/2 mouse → `/dev/mouse`** — kernel mouse driver (below) exposes a readable
   `/dev/mouse` device returning parsed events `{ dx, dy, buttons }`; plus a
   `mouse_inject_packet()` hook so tests can drive it headlessly (mirrors
   `keyboard_inject_text`).

Keyboard already reaches the WM: the WM is the foreground task, so it reads keys
via the normal per-task keyboard ring / `SYS_READ(0)`. It then routes them to the
focused tab's shell over a pipe.

## Hosting the real shell in a tab (the crux)

Each tab is a `sh.elf` child the WM spawns and pumps:

```
WM (wm.elf)
  fork()
  child: dup2 pipes → fd 0/1/2, execve("/apps/sh.elf", ...)   # the real login shell
  parent: holds (in_pipe_w, out_pipe_r) for this tab

per frame / on input:
  window keystrokes  → write(in_pipe_w)   → sh.elf stdin
  read(out_pipe_r)   → terminal widget    → tab's glyph grid → window pixels
```

- Reuses existing `fork`/`execve`/`pipe`/`dup2`. No new process ABI.
- The **terminal widget** is a per-tab char grid (cols×rows of `{ch,fg,bg}`) with
  a minimal VT parser (CR/LF/BS, later a small ANSI subset). It renders glyphs
  from `FONT8x8`/`FONT8x16` into the tab's pixel region of the window.
- The shell prints its `mak.sh` prompt (current user) into the widget unchanged —
  it just sees a pipe as its tty.
- **Tabs are per-window**: a window holds `tab[]`, each with its own grid +
  `sh.elf` child + pipes; only the active tab renders, a tab strip across the top
  of the window switches focus. New-tab / close-tab buttons on the strip.

## Compositor (double buffering)

- WM allocates one back buffer `W*H*4` via anon `mmap2` in the `0x90000000`
  window (720p×4 ≈ 3.5 MiB — confirm the mmap window / user brk has room).
- Per frame, back-to-front: desktop bg → desktop icons → windows by z-order
  (each: title bar, tab strip, active tab grid, border) → bottom status bar →
  software mouse cursor. Then `SYS_FB_PRESENT` the whole buffer.
- Full-frame redraw first; add dirty rectangles once it's correct.

## PS/2 mouse driver (kernel)

- Enable the aux device on the 8042: `0xA8` (enable aux), `0x20/0x60` to set the
  "enable IRQ12" bit in the controller config, `0xD4`-prefixed `0xF4` (enable
  data reporting) to the device.
- IRQ12 handler assembles the 3-byte packet (sync on the always-1 bit of byte 0),
  decodes sign/overflow + L/M/R buttons, pushes `{dx,dy,buttons}` to a ring.
- `/dev/mouse` read drains the ring; `mouse_inject_packet()` feeds the same ring
  for tests. Keep it SMP-comment-clean like `keyboard.c`.

## Testing (headless, per the project rule — no host input, no FB scraping)

- **Mouse driver**: ktest drives `mouse_inject_packet()` and asserts decoded
  `{dx,dy,buttons}` over serial — mirrors the `keyboard_inject_text` pattern.
- **Compositor**: composite into an in-RAM buffer and CRC/sample known pixels,
  emitting the result over serial (no scanout scraping).
- **WM behaviour**: `wm.elf` emits `WM:` serial markers (window opened/moved/
  closed, icon hit, tab switch). A test driver injects a mouse path
  (move→press→drag→release) and asserts the markers — analogous to
  `keyboard_test_driver()`. Add to the `.sh` drivers / a new `wmtest` section,
  never a host harness.

## Slice queue

Build foundation-up; each slice is independently shippable and headlessly tested.

| # | Slice | Deliverable |
|---|---|---|
| G1 | **PS/2 mouse driver** | aux+IRQ12 enable, packet decode, `/dev/mouse`, `mouse_inject_packet()`, ktest |
| G2 | **FB present ABI** | `SYS_FB_INFO2` (pitch/bpp) + `SYS_FB_PRESENT(buf,x,y,w,h)`, ktest blit-and-verify |
| G3 | **Userspace gfx lib** | ring-3 back-buffer alloc + rectfill/blit/glyph(FONT8x8)/line; present-and-CRC test |
| G4 | **WM core + compositor** | `wm.elf`: event loop (`/dev/mouse`+kbd), window list, z-order, full-frame composite, sw cursor; `WM:` markers |
| G5 | **Terminal window + tabs** | per-tab `sh.elf` via fork/exec/pipe; terminal widget grid; per-window tab strip (new/close/switch) |
| G6 | **Desktop + icon + status bar** | desktop bg, terminal icon (click → new terminal window), bottom status bar mirroring makmux (user/clock) |
| G7 | **`gui` launcher** | shell command execs `wm.elf`; restore text console on WM exit |

Open items to confirm during G2/G4: mmap window headroom for a 3.5 MiB back
buffer; whether `SYS_FB_PRESENT` takes a user pointer + bounds-checks it, or the
WM maps the LFB directly later for zero-copy.
