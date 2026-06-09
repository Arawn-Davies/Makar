---
title: video
parent: Kernel subsystems
grand_parent: Reference
---

# video - display-driver framework (vtable + accelerated backends)

**Header:** `kernel/include/kernel/video.h`
**Sources:** `arch/i386/display/video.c`, `video_vbe.c`, `video_svga2.c`

The kernel owns **one active display driver** behind a small vtable
(`vid_driver_t`). The default driver (`video_vbe`) is the dumb
linear-framebuffer path — present = a per-line `memcpy` into the LFB plus a
software-composited cursor — which covers QEMU-std, Bochs/DISPI, VirtualBox VGA,
and bare metal. Accelerated drivers bind **ahead** of it when their hardware is
present, advertising 2D copy and/or a hardware cursor through `caps`.

Everything that used to touch the LFB directly — `SYS_FB_PRESENT[_RECT]` and the
WM cursor — now routes through `video_active()`, so a driver can intercept the
blit (issue a 2D `UPDATE` instead of a CPU copy) and own the cursor. All drivers
share the framebuffer geometry from `vesa_get_fb()`; the back buffer a client
presents is full-frame, tightly packed 32-bpp (pitch = `width*4`).

---

## Capability bits

| Bit | Meaning |
|---|---|
| `VID_CAP_ACCEL_COPY` | `present_rect` is a real 2D op, not a CPU `memcpy`. |
| `VID_CAP_HW_CURSOR` | `cursor_*` drive a real hardware sprite. |

## The vtable — `vid_driver_t`

| Field | Role |
|---|---|
| `probe()` | Non-zero if this driver should bind on this machine. Called in registry order; first match wins, else the default LFB driver. |
| `init()` | Bring the device up to the current `vesa_get_fb()` geometry. Non-zero ⇒ fall back to the next candidate. |
| `set_mode(w,h)` | Switch to `(w,h)x32` at runtime via the device's own registers and repoint the framebuffer. `NULL` on backends that can't mode-set (plain VBE LFB) ⇒ caller falls back to the Bochs DISPI path. |
| `present_rect(src,x,y,w,h)` | Present a rect of a full-frame packed-32bpp back buffer to the screen. |
| `flush_rect(x,y,w,h)` | Scan out a rect the caller wrote **directly** into the framebuffer (no copy). `NULL` for live-framebuffer backends; SVGA II needs it because direct writes aren't visible until an explicit `UPDATE`. The text console flushes through here. |
| `cursor_define/move/show` | Hardware cursor (only with `VID_CAP_HW_CURSOR`). |

## Public API

- `video_init()` — walk the registry, bind the first driver whose `probe()` then
  `init()` passes, else the LFB driver. Call once after `vesa_init()` **and**
  after the PCI scan (an accelerated backend needs the device enumerated).
  Under the `sysadmin`/`hwspecs` text modes this bind is deferred (see
  [boot modes](../internals.md)).
- `video_active()` — the bound driver (never `NULL` after `video_init`).
- `video_caps()` — capability bits of the active driver.
- `video_present(src)` / `video_present_rect(src,x,y,w,h)` — used by
  `SYS_FB_PRESENT[_RECT]`; route to the active driver.
- `video_flush_rect(x,y,w,h)` — scan out a rect already written into the FB
  (text-console path); no-op on live-framebuffer backends.
- `video_set_mode(w,h)` — ask the active driver to mode-set via its own
  registers (SVGA II); returns non-zero if it can't, so the caller falls back to
  Bochs DISPI ([bochs_vbe](bochs_vbe.md)). The `mxdisplay` app and the `setmode`
  shell command drive this.
- `video_svga2_to_vga()` — **panic path**: disable the SVGA II engine so the
  device reverts to VGA-compatible scanout (the panic then programs VGA mode 3 +
  writes `0xB8000`). No-op if SVGA II never bound.

---

## Backends

- **`video_vbe`** — the default LFB driver. `present_rect` = per-line `memcpy`;
  software cursor; live framebuffer (no `flush_rect`); no `set_mode` (mode
  changes go through Bochs DISPI). Always binds last.
- **`video_svga2`** — VMware/VirtualBox **SVGA II** (PCI `15AD:0405`). FIFO
  command ring + `SVGA_CMD_UPDATE`; FIFO cursor-bypass-3 hardware cursor;
  arbitrary mode-set via the SVGA registers (snapped to a clean standard mode by
  `svga_pick_mode` so a mismatched width/height can't slip through). Advertises
  both capability bits. See `video_svga2.c`.
- **Hyper-V synthvid** — a registry slot exists; the VMBus backend is not yet
  implemented (Hyper-V runs fine on the VBE LFB path).

See also: [vesa](vesa.md) (the shared framebuffer + geometry), [bochs_vbe](bochs_vbe.md)
(the DISPI mode-set fallback), [pci](pci.md) (device binding), and `docs/gui.md`.
