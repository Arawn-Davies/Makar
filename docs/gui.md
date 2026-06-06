# Makar GUI / windowing

The Makar desktop (`gui.elf`, `src/userspace/wm.c`) is a userspace window
manager that owns the framebuffer and composites a desktop, a dock, and a set
of windows. This document describes the windowing architecture as it is built
out; it is updated per phase.

## Design constraints

Two kernel facts shape the whole design:

1. **The WM is the sole compositor.** `SYS_FB_PRESENT` / `SYS_DRAW_LINE` are
   focus-gated (`vtty_is_focused`, `proc/vtty.c`). The WM is the single focused
   root-GUI task, so only it may blit the framebuffer. Every window — including
   graphical children like DOOM — renders into an off-screen buffer that the WM
   reads and composites. Nothing else touches scanout.

2. **No userspace shared memory except surfaces.** `fork` is copy-on-write and
   `SYS_MMAP2` is private-anon, so a forked child cannot hand pixels back to the
   WM through ordinary memory. The kernel **shared surface** primitive
   (below) is the bridge.

Because the WM owns input and composites everything, **keyboard/mouse focus is
managed entirely inside the WM** — the kernel focus model is untouched. The WM
holds kernel focus; it routes input to the active window. Clicking a window
makes it active (keyboard target) and raises it; the window under the cursor
receives mouse motion.

## Shared pixel surfaces (kernel)

`kernel/surface.h` + `arch/i386/proc/surface.c`. A surface is a kernel-owned run
of physical frames mappable into several tasks at once (same frames, distinct
virtual addresses). Syscalls: `SYS_SURFACE_CREATE/MAP/INFO/DESTROY` (see
`docs/syscalls.md`).

Lifetime is reference-counted: alive while the creator ref is held **or** any
task still maps it. `surface_release_task()` runs from `task_terminate()` and
unmaps a dying task's surface pages from its page directory *before*
`vmm_free_pd()` walks it — without this the shared frames, still mapped by
another holder, would be double-freed. Covered by the `surface` ktest suite.

Usage pattern for a windowed graphical app:

```
WM:    id = surface_create(W, H); base = surface_map(id);   // composite from base
       fork(); child execve("app", "-surface", id, ...);    // + stdin key pipe
child: base = surface_map(id); /* render frames into base; never fb_present */
WM:    each frame: blit the surface into the app's window rect
       on child exit: surface_destroy(id)
```

## Widget framework (userspace)

`src/userspace/gui_gfx.{c,h}` + `gui_ui.{c,h}` (flat in `src/userspace/`, since
the userspace build is single-directory `-nostdinc -I.`). Linked into `gui.elf`
and reusable by future surface-rendering apps.

- **gui_gfx** — a `gfx_surface` (XRGB8888 pixel buffer + w/h) and clipped
  primitives: `gfx_px/fill/outline/round/char/str/str_clip`, `gfx_blit` (region
  copy, for compositing window contents) and `gfx_blit_scaled` (nearest-
  neighbour, for fitting a fixed app surface like DOOM's 640x400 into a window).
- **gui_ui** — an immediate-mode toolkit. Each frame the caller snapshots input
  with `ui_begin` then calls widgets in a fixed order; widget identity is the
  call order. Widgets: `ui_button`, `ui_slider`, `ui_textbox`, `ui_label`,
  `ui_listbox`. Transient interaction (pressed/dragged widget, keyboard focus)
  lives in `ui_ctx`; all content is caller-owned. The window manager hit-tests
  windows first and only feeds the focused window a "live" ctx (others get a ctx
  with no buttons/keys so they still draw but don't react) — this is how the
  per-window focus model reaches individual widgets.

## Window manager (`wm.c`)

`gui.elf` keeps a fixed window per kind (`W_TERMINAL/EDITOR/FILES/TASKS/DOOM`)
with a z-order list. Each frame it: gathers mouse + one key, does
window-management click handling (dock/taskbar, icons, raise+focus, title-bar
drag, close box), routes the key to the focused window (terminal/doom over a
pipe; editor/files/tasks via their `ui_ctx` pass), then composites desktop →
windows back-to-front → dock → cursor and presents once. Only the focused
window receives a "live" `ui_ctx`; others draw but don't react. `gui uitest`
runs a headless widget self-test emitting `GUI-UITEST: PASS`.

- **Terminal** hosts `sh.elf` over pipes (byte stream drawn as a grid). It is a
  *forked* shell on a private pipe, so Ctrl-C / Ctrl-D there only ever closes
  that terminal window — `mak.sh0` (the login session) is never in the blast
  radius.
- **Editor** is a native multi-line editor (caret, click-to-position) with an
  **Open / Save / Save As** dialog built on the shared file browser (below):
  Save As lets you traverse to a directory and type a filename.
- **Files** is a native browser built on the same reusable `browser` model:
  **Up / Open / Refresh** plus a path box + **Go** to jump to an absolute path;
  `.`/`..` are hidden (Up handles the parent). Opening a file routes it to the
  Editor window. Navigation captures the post-`chdir` cwd via `getcwd` before
  reloading (an earlier bug reset the cwd on every reload — see the `fstest`).
- **Tasks** parses `/proc/tasks`, Kill via `SYS_KILL`, refresh-interval slider.
- **Doom** forks `doom.elf -surface <id>`; the WM maps the shared surface and
  `gfx_blit_scaled`s it into the window, forwarding keys over the child's stdin.

An always-on **top menu bar** (drawn after the windows every frame, so it is
never occluded) carries the Makar brand, the focused window's name, and the
**Log Off** item. Log Off tears down the WM's children (terminal / doom),
restores terminal + statusbar state, calls `sys_logout()` to end the underlying
login session, and exits — returning to the login screen with `mak.sh0` intact.
Desktop + menu bar + dock are unconditional: the GUI is never chromeless.

### Graphical login

Launched as `gui login` (the kernel does this when no user is auto-logged-in),
`gui.elf` shows a graphical login (`do_login`): username + masked password
(`ui_password`) + a Log In button. Submit calls **`SYS_LOGIN`** (`sys_login`,
syscall 266) → `auth_login` → `shadow_verify` + set the session user; on success
it falls through to the desktop. This is the only new syscall in the GUI work;
when the GUI is split into a display server + clients (next PR), `do_login`
lifts wholesale into a standalone `login.elf`.

### Booting straight to the desktop / login

The `autoboot=gui` kernel cmdline token makes `shell_login_loop` hand
authentication to the GUI: it tries autologin once, then passes
`--autostart=gui` (auto-logged-in → desktop) or `--autostart=gui-login` (→ the
graphical login) to the login `sh.elf`, which runs `gui` / `gui login` after
sourcing `~/.makshrc`. The desktop runs inside the login session (which stays
the GUI's parent, so Log Off returns to it cleanly).

Bootloader entries:
- **GRUB (live ISO):** `Makar OS` / `Makar OS (GUI desktop)` (`live autoboot=gui`,
  the **default** entry) / `rescue shell` / `serial console`. `GRUB_DEFAULT`
  overrides the auto-selected entry (the kbtest/guitest harnesses pin it to 0).
- **Limine (installed):** the installer builds `limine.conf` with a
  `/Makar OS (GUI desktop)` entry whose cmdline embeds the configured autologin
  user (`autologin=<user> autoboot=gui`), plus a rescue entry.

### Testing the GUI boots: `./run.sh guitest`

Boots straight into the desktop (autologin), waits for `gui.elf`'s `GUI: READY`
serial marker (emitted after the first composited frame is presented), captures
a QEMU **screendump** (PPM → `gui-screendump.bmp` via `tests/ppm2bmp.py`), then
shuts down. The marker is the pass gate; the screendump is a best-effort viewable
artifact. This is the end-to-end "the window server actually starts and draws"
check that the headless `uitest`/`fstest` (which never touch the framebuffer)
cannot give.

### Reusable file browser + headless coverage

The Files window and the Editor's Open/Save dialog share one `browser` model
(`cwd` + entries + select/enter/up), navigating via the existing
`chdir`/`readdir`/`getcwd` syscalls. `gui fstest` drives that model against the
live VFS (descend a real dir, confirm the cwd deepened, climb back) and emits
`GUI-FSTEST: PASS`; it runs under `shell-smoke.sh` alongside `gui uitest`
(`GUI-UITEST: PASS`). Both return before touching the framebuffer, so they are
headless-safe — this is the automated proof that "Files actually moves about the
filesystem" without needing pixels.

## DOOM windowed backend (`doomgeneric_makar.c`)

`-surface <id>` selects windowed mode: `DG_Init` maps the surface instead of the
full framebuffer, `DG_DrawFrame` writes the frame into the surface and does
**not** call `SYS_FB_PRESENT` (the WM composites), and input comes from stdin
(the WM forwards decoded key bytes). Since cooked stdin has no key-up codes, a
press auto-releases after ~120 ms (tap-to-move) — good enough for menus/turning,
a known limitation. Without `-surface`, DOOM runs its normal fullscreen path
(shell `doom`), unchanged.

## Status

- **Phase 1 (done):** kernel shared surfaces + ktest.
- **Phase 2 (done):** userspace GUI widget framework (`gui_gfx`, `gui_ui`).
- **Phase 3 (done):** multi-window WM + click-to-focus input routing.
- **Phase 4 (done):** native Editor / Files / Task-manager windows, now with a
  working file browser (Up/Open/Go), an Editor Open/Save/Save-As dialog on the
  shared browser, and a dock **Log Off**. Navigation is regression-covered by
  `gui fstest` (`GUI-FSTEST: PASS`).
- **Phase 5 (done):** DOOM in a window via a shared surface.
- **Phase 6 (todo):** host drag-and-drop GUI designer that emits widget-layout
  code. Deferred — the widget schema (`gui_ui.h`) is the contract it will target.

## Next PR (not this one): trim the syscall surface

The GUI leans on many bespoke syscalls (`FB_PRESENT`, `DRAW_LINE`, `MOUSE_READ`,
`SURFACE_*`, `WHOAMI`, `STATUSBAR`, `WRITE_FILE`, `WRITE_SERIAL`, `KEYBOARD_RAW`)
that on real Linux are device files + ioctl (`/dev/fb0`, `/dev/input/*`), `getuid`,
`open`/`write`, `/dev/kmsg`, termios. Reducing the ~101-syscall surface toward
those idioms is the next "proper kernel" PR; GUI changes here were kept
syscall-neutral so that rework isn't pre-empted.

## Verification status

- **Navigation** (Files / Editor dialog) is headlessly proven by `gui fstest`.
- **Widgets** by `gui uitest`.
- **The desktop boots and draws** by `./run.sh guitest` — `GUI: READY` plus a
  1280×720 screendump whose colours are exactly the WM palette (desktop bg,
  window, title bar, menu bar, icons).

Still wanting a human pass (inherently interactive): click-to-focus *feel*, the
Editor Save-As dialog, the graphical login keyboard flow, and the Doom surface
blit/playability. `./run.sh iso boot` and pick **Makar OS (GUI desktop)**.
