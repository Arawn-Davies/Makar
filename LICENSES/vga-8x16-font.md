# VGA 8×16 Bitmap Font

**File:** `src/kernel/include/kernel/vga_8x16_font.h`

## License / Origin

The `FONT8x16` glyph table reproduces the IBM PC VGA 8×16 character ROM
glyph set as dumped in the Linux kernel's `lib/fonts/font_8x16.c`.

The Linux kernel is GPLv2. The 8×16 ROM glyph set itself originated in
the IBM VGA BIOS, which has been freely distributable since the early
1990s and is reproduced verbatim across open-source operating systems
(Linux, FreeBSD, OpenBSD, FreeDOS, GRUB, etc.) — it is widely treated
as **public domain**.

This Makar copy was retyped from the Linux `font_8x16.c` table.  No
Linux kernel source code is statically linked or otherwise included in
the Makar build; the table is data only, used by Makar's own VGA
text-mode font upload routine in
`src/kernel/arch/i386/display/bochs_vbe.c`.

## Gratitude

Thanks to:

- **IBM** for releasing the VGA character ROM glyphs to the public.
- **The Linux kernel project** (`lib/fonts/font_8x16.c` maintainers,
  past and present) for keeping the canonical public-domain copy
  freely available.
- **FreeDOS** and **FreeBSD** for parallel verification of the same
  glyph data, used as cross-check during retyping.

## Description

256 entries, one per CP437 code point. Each entry is 16 bytes (16
scanlines tall). Within each byte, bit 7 is the leftmost pixel and
bit 0 is the rightmost (standard IBM/VGA MSB-first order).

Entries 0x00–0x7F carry the ASCII printable glyphs.  Entries 0x80–0xFF
are reserved for the extended CP437 set and currently render as blank
glyphs — Makar's kernel TTY does not emit those code points anywhere
today.

The font is uploaded to VGA plane 2 by `vga_load_text_font()` in
`bochs_vbe_disable()` so that `setmode 80x25` and `setmode 80x50`
display readable text after a VESA session.
