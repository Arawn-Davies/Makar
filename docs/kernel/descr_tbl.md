# descr_tbl - GDT and IDT initialisation

**Header:** `kernel/include/kernel/descr_tbl.h`  
**Source:** `kernel/arch/i386/core/descr_tbl.c`  
**ASM stubs:** `kernel/arch/i386/core/dt_asm.S`

Sets up the two fundamental x86 descriptor tables required before the CPU can
run protected-mode code correctly: the Global Descriptor Table (GDT) and the
Interrupt Descriptor Table (IDT).

**OSDev references:**
- [Global Descriptor Table (GDT)](https://wiki.osdev.org/GDT)
- [GDT Tutorial (flush + far-jump)](https://wiki.osdev.org/GDT_Tutorial)
- [Interrupt Descriptor Table (IDT)](https://wiki.osdev.org/IDT)
- [8259 PIC remapping](https://wiki.osdev.org/8259_PIC)
- [Task State Segment (TSS)](https://wiki.osdev.org/TSS)

---

## Data structures

### `gdt_entry_t`

```c
struct gdt_entry_struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));
```

One 8-byte entry in the GDT.  Each entry describes a memory segment: its base
address, limit, privilege level, and flags.  The `packed` attribute prevents
the compiler from inserting alignment padding.  See the
[GDT OSDev article](https://wiki.osdev.org/GDT) for the full bit layout of
the `access` and `granularity` bytes.

### `gdt_ptr_t`

```c
struct gdt_ptr_struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));
```

The 6-byte pseudo-descriptor passed to the `LGDT` instruction.  `limit` is
`(sizeof(gdt_entry_t) * count) - 1`; `base` is the linear address of the
first GDT entry.

### `idt_entry_t`

```c
struct idt_entry_struct {
    uint16_t base_lo;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_hi;
} __attribute__((packed));
```

One 8-byte interrupt gate descriptor.  `base_lo`/`base_hi` together form the
32-bit address of the ISR stub; `sel` is the kernel code segment selector
(`0x08`); `flags` encodes the gate type and privilege level (`0x8E` = present,
ring-0 interrupt gate; `0xEE` = present, ring-3 interrupt gate for syscalls).
See the [IDT OSDev article](https://wiki.osdev.org/IDT) for the full bit layout.

### `idt_ptr_t`

```c
struct idt_ptr_struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));
```

The 6-byte pseudo-descriptor passed to the `LIDT` instruction.

### `tss_t`

A packed struct holding the 32-bit Task State Segment fields.  Only `esp0`,
`ss0`, and `iomap_base` are used:

| Field | Value | Purpose |
|---|---|---|
| `ss0` | `0x10` | Kernel data segment for the ring-0 stack. |
| `esp0` | updated per task-switch | Kernel stack pointer for ring 3 → ring 0 transitions. |
| `iomap_base` | `sizeof(tss_t)` | Points past the TSS - no I/O permission bitmap. |

See the [TSS OSDev article](https://wiki.osdev.org/TSS) for the complete field list.

---

## GDT layout

Six descriptors are installed at boot:

| Index | Selector | Type | Description |
|---|---|---|---|
| 0 | `0x00` | Null | Required null descriptor. |
| 1 | `0x08` | Code | Kernel code segment (ring 0, 0–4 GiB, 32-bit). |
| 2 | `0x10` | Data | Kernel data segment (ring 0, 0–4 GiB). |
| 3 | `0x18` | Code | User code segment (ring 3, 0–4 GiB, 32-bit). |
| 4 | `0x20` | Data | User data segment (ring 3, 0–4 GiB). |
| 5 | `0x28` | TSS  | 32-bit available TSS (access byte `0x89`). |

All segments use a flat base of 0 and limit of 0xFFFFFFFF (4 GiB).
Protection is handled entirely by the paging unit.

---

## IDT layout

256 gates are installed.  Vectors 0–31 map to the CPU exception stubs
(`isr0`–`isr31`); vectors 32–47 map to the hardware IRQ stubs
(`irq0`–`irq15`) after the 8259 PIC has been remapped (see
[8259 PIC OSDev article](https://wiki.osdev.org/8259_PIC)).

Vector 128 (`0x80`) is the syscall gate: installed with `flags = 0xEE`
(DPL=3) so that user-mode code can invoke it with `int 0x80`.

---

## Functions

### `init_descriptor_tables`

```c
void init_descriptor_tables(void);
```

Public entry point called from `kernel_main`.  Initialises the GDT (including
the TSS descriptor), loads it with `gdt_flush` and `tss_flush`, initialises
the IDT (including remapping the 8259 PIC and installing the syscall gate),
loads it with `idt_flush`, then registers the ISR dispatch stubs via
`init_isr_handlers()`.

### `tss_set_kernel_stack`

```c
void tss_set_kernel_stack(uint32_t esp0);
```

Update the `ESP0` field in the TSS.  Must be called before every ring-3 →
ring-0 transition (i.e. on every task switch) to point the CPU at the correct
per-task kernel stack.
