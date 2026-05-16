---
title: vesa
parent: Kernel subsystems
---

# vesa - VESA linear framebuffer driver

**Header:** `kernel/include/kernel/vesa.h`  
**Source:** `kernel/arch/i386/vesa.c`  
**Config:** `kernel/include/kernel/vesa_config.h`

Locates the Multiboot 2 framebuffer tag and exposes the linear framebuffer
for pixel-level drawing.  The GRUB Multiboot 2 header (in `boot.S`) requests
a direct-colour RGB framebuffer at a preferred resolution defined in
`vesa_config.h`.

---

## Configuration (`vesa_config.h`)

| Macro | Resolved by | Description |
|---|---|---|
| `VESA_PREFERRED_WIDTH` | `VESA_WIDTH` | Requested horizontal resolution in pixels. |
| `VESA_PREFERRED_HEIGHT` | `VESA_HEIGHT` | Requested vertical resolution in pixels. |
| `VESA_PREFERRED_BPP` | `VESA_BPP` | Requested bits per pixel (typically 32). |

The bootloader may supply a different resolution if the hardware does not
support the preferred one; code must always use the values from `vesa_fb_t`,
not the compile-time constants.

---

## Data structures

### `vesa_fb_t`

```c
typedef struct {
    uint32_t *addr;
    uint32_t  pitch;
    uint32_t  width;
    uint32_t  height;
    uint8_t   bpp;
    uint8_t   red_shift;
    uint8_t   red_bits;
    uint8_t   green_shift;
    uint8_t   green_bits;
    uint8_t   blue_shift;
    uint8_t   blue_bits;
} vesa_fb_t;
```

Describes the framebuffer as reported by the bootloader.  The RGB shift and
bit-width fields are used to compose pixel values for any packed-pixel
format; see `vesa_put_pixel`.

---

## Functions

### `vesa_init`

```c
bool vesa_init(multiboot2_info_t *mbi);
```

Walk the Multiboot 2 tag list to find the framebuffer tag (type 8).  If
found and the framebuffer type is direct-colour RGB, populate the global
`vesa_fb_t` descriptor.

Returns `true` on success.  Returns `false` and logs a reason if:
- `mbi` is `NULL`.
- No framebuffer tag is present.
- The framebuffer is not direct-colour RGB (e.g. EGA text or indexed colour).
- `bpp` is zero or not byte-aligned.

### `vesa_get_fb`

```c
const vesa_fb_t *vesa_get_fb(void);
```

Return a pointer to the global framebuffer descriptor, or `NULL` if
`vesa_init()` has not been called successfully.  Read-only; callers must not
modify the returned struct.

### `vesa_put_pixel`

```c
void vesa_put_pixel(uint32_t x, uint32_t y, uint32_t colour);
```

Write a pixel at (`x`, `y`).  `colour` is a 32-bit packed value in
`0x00RRGGBB` format; the function shifts and masks the channels to match the
framebuffer's reported layout before writing the correct number of bytes.

**No bounds checking is performed.**  Out-of-bounds coordinates will corrupt
adjacent framebuffer memory or crash the system.

### `vesa_clear`

```c
void vesa_clear(uint32_t colour);
```

Fill the entire framebuffer with a single colour by calling `vesa_put_pixel`
for every pixel.  Used by `vesa_tty_init()` to clear the screen.

---

## Future work

- Hardware-accelerated `memset`-style fill using `REP STOSD` for faster
  clears and rectangle fills.
- Support for `bpp` values other than 32 (e.g. 16-bit 5-6-5 colour).
- Double-buffering: render to an off-screen buffer and flip to reduce
  visible tearing.
- Mode-setting: negotiate resolution with the firmware at runtime rather
  than relying solely on the Multiboot 2 framebuffer tag.
