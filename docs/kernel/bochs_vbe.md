---
title: bochs_vbe
parent: Kernel subsystems
grand_parent: Reference
---

# bochs_vbe - Bochs VBE / QEMU BGA (DISPI) mode-setting

**Header:** `kernel/include/kernel/bochs_vbe.h`
**Source:** `arch/i386/display/bochs_vbe.c`

The Bochs VBE / QEMU "Bochs Graphics Adapter" (DISPI) interface, accessible via
**I/O ports** from protected mode — no real-mode BIOS calls. This is how Makar
sets an arbitrary linear-framebuffer resolution at boot and at runtime on the
QEMU-std / Bochs / VirtualBox-VGA adapters. VMware/VirtualBox SVGA II mode-sets
go through that driver's own registers instead (see [video](video.md)); DISPI is
the fallback the framework reaches for when `video_set_mode` returns "can't".

---

## API

| Function | Role |
|---|---|
| `bochs_vbe_available()` | True if a Bochs-compatible VBE adapter is detected (DISPI id at port `0x1CE/0x1CF`). |
| `bochs_vbe_vram_bytes()` | Total video memory (DISPI `VIDEO_MEMORY_64K`); `0` if not reported. |
| `bochs_vbe_caps(&w,&h,&bpp)` | Max mode the adapter advertises (DISPI GETCAPS). Any out-param may be `0`. |
| `bochs_vbe_mode_supported(w,h,bpp)` | True if `w×h×bpp` fits both the dimension caps **and** the VRAM. Gates the boot-resolution pick and `setmode` so we never program a mode the adapter can't scan out (black screen / FB overrun). |
| `bochs_vbe_set_mode(w,h,bpp)` | Switch the LFB to `w×h` at `bpp`. `LFB_ENABLED` keeps the framebuffer address constant. Call `vesa_update_geometry()` + `vesa_tty_init()` after. |
| `bochs_vbe_disable()` | Disable VBE graphics; QEMU reverts to VGA text mode 3 (80×25) and the 8×16 IBM VGA ROM font is uploaded to plane 2. |
| `vga_load_text_font_8x8()` | Replace the plane-2 font with the kernel's 8×8 glyphs (for 80×50, where the CRTC reads only the first 8 bytes of each glyph). |

---

## Boot use

`kernel_main` picks the boot resolution from a `vmode=` cmdline request (or the
**720p** default), gated through `bochs_vbe_mode_supported()` so an unsupported
ask falls back cleanly — 1280×720 → 1024×768 → 640×480 → the adapter max. The
panic path calls `bochs_vbe_disable()` (plus `video_svga2_to_vga()`) to get back
to legible VGA text before rendering. See [vesa](vesa.md), [video](video.md), and
[boot modes](../internals.md).
