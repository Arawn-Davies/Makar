---
title: multiboot
parent: Kernel subsystems
---

# multiboot - Multiboot 2 structure definitions

**Header:** `kernel/include/kernel/multiboot.h`

Type definitions and constants for the Multiboot 2 boot protocol.  Makar is
loaded by GRUB, which places a pointer to the Multiboot 2 information
structure in the `ebx` register before transferring control to `_start`.

---

## Magic value

```c
#define MULTIBOOT2_BOOTLOADER_MAGIC  0x36D76289
```

The bootloader places this value in `eax` at entry.  `kernel_main` passes it
to `pmm_init()`, which validates it before trusting the memory map.  The GDB
boot test also verifies this value at `_start`.

---

## Tag type constants

| Constant | Value | Description |
|---|---|---|
| `MULTIBOOT2_TAG_TYPE_END` | 0 | Terminates the tag list. |
| `MULTIBOOT2_TAG_TYPE_MMAP` | 6 | Physical memory map. |
| `MULTIBOOT2_TAG_TYPE_FRAMEBUFFER` | 8 | Linear framebuffer info. |

---

## Memory type constants

| Constant | Value | Description |
|---|---|---|
| `MULTIBOOT2_MEMORY_AVAILABLE` | 1 | RAM usable by the OS. |

---

## Framebuffer type constants

| Constant | Value | Description |
|---|---|---|
| `MULTIBOOT2_FRAMEBUFFER_TYPE_INDEXED` | 0 | Palette/indexed colour. |
| `MULTIBOOT2_FRAMEBUFFER_TYPE_RGB` | 1 | Direct/packed RGB colour. |
| `MULTIBOOT2_FRAMEBUFFER_TYPE_EGA_TEXT` | 2 | EGA text mode. |

---

## Structures

### `multiboot2_info_t`

```c
typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} __attribute__((packed)) multiboot2_info_t;
```

The 8-byte header at the very start of the information structure.
`total_size` includes the header itself.  Tags begin at
`(uint8_t *)mbi + sizeof(multiboot2_info_t)`.

### `multiboot2_tag_t`

```c
typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) multiboot2_tag_t;
```

Generic tag header.  All tags begin with these two fields.  To advance to
the next tag: `ptr += (tag->size + 7) & ~7` (tags are 8-byte aligned).

### `multiboot2_mmap_entry_t`

```c
typedef struct {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed)) multiboot2_mmap_entry_t;
```

One entry in the physical memory map.  `type == MULTIBOOT2_MEMORY_AVAILABLE`
identifies usable RAM.

### `multiboot2_tag_mmap_t`

```c
typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    /* multiboot2_mmap_entry_t entries[] follow */
} __attribute__((packed)) multiboot2_tag_mmap_t;
```

Memory map tag (type 6).  Individual entries begin at
`(uint8_t *)tag + sizeof(multiboot2_tag_mmap_t)` and are each `entry_size`
bytes apart.

### `multiboot2_tag_framebuffer_t`

```c
typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    /* RGB channel layout fields follow for type == RGB */
    uint8_t  red_field_position;
    uint8_t  red_mask_size;
    uint8_t  green_field_position;
    uint8_t  green_mask_size;
    uint8_t  blue_field_position;
    uint8_t  blue_mask_size;
} __attribute__((packed)) multiboot2_tag_framebuffer_t;
```

Framebuffer tag (type 8).  `framebuffer_addr` is the physical base address
of the linear framebuffer.  The RGB shift and mask fields describe how to
compose a pixel value for the given `bpp`.
