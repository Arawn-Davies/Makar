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

## Status

- **Phase 1 (done):** kernel shared surfaces + ktest.
- **Phase 2 (done):** userspace GUI widget framework (`gui_gfx`, `gui_ui`).
- Phase 3: multi-window WM + click-to-focus input routing.
- Phase 4: native Editor / Files / Task-manager windows.
- Phase 5: DOOM in a window via a shared surface.
- Phase 6: host drag-and-drop GUI designer that emits widget-layout code.
