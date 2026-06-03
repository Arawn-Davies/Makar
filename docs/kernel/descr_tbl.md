---
title: descr_tbl
parent: Kernel subsystems
grand_parent: Reference
---

# descr_tbl - GDT, IDT, TSS, and TLS descriptors

**Header:** `kernel/include/kernel/descr_tbl.h`  
**Source:** `kernel/arch/i386/core/descr_tbl.c`  
**ASM stubs:** `kernel/arch/i386/core/dt_asm.S`

Sets up the x86 descriptor tables that Makar relies on after GRUB hands the
kernel a protected-mode CPU: the Global Descriptor Table (GDT), the Interrupt
Descriptor Table (IDT), the Task State Segment (TSS), and the single user TLS
descriptor used by the Linux i386 `set_thread_area` ABI.

Makar uses a flat segmentation model for normal code and data. Paging provides
memory isolation; segmentation exists because x86 still needs selectors for
ring transitions, stack switching, syscall entry, and `%gs`-based thread-local
storage in hosted libc binaries.

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

Seven descriptors are installed at boot:

| Index | Selector | Type | Description |
|---|---|---|---|
| 0 | `0x00` | Null | Required null descriptor. |
| 1 | `0x08` | Code | Kernel code segment (ring 0, 0–4 GiB, 32-bit). |
| 2 | `0x10` | Data | Kernel data segment (ring 0, 0–4 GiB). |
| 3 | `0x18` | Code | User code segment (ring 3, 0–4 GiB, 32-bit). |
| 4 | `0x20` | Data | User data segment (ring 3, 0–4 GiB). |
| 5 | `0x28` | TSS  | 32-bit available TSS (access byte `0x89`). |
| 6 | `0x33` | TLS data | User data segment programmed by `set_thread_area`; selector includes RPL 3. |

The normal code/data segments use a flat base of 0 and a 4 GiB limit.
Protection for memory accesses is handled by paging, not by shrinking segment
limits. The TLS entry is the exception: it is reprogrammed per task so `%gs:0`
can refer to the calling task's thread pointer.

### User TLS slot

The TLS descriptor is defined by:

```c
#define GDT_TLS_INDEX    6
#define GDT_TLS_SELECTOR 0x33u
```

`SYS_SET_THREAD_AREA` accepts a Linux-style `struct user_desc`, validates that
the caller is asking for either `-1` or entry 6, and then calls
`gdt_set_tls(base, limit, limit_in_pages, present)`. The syscall records the
same TLS fields in `task_t` so the scheduler can restore the descriptor on the
next context switch.

The kernel intentionally leaves `%gs` alone in the interrupt stubs. User code
may have `%gs == 0x33`, and an interrupt or syscall must not destroy that
selector merely because the CPU entered ring 0. Kernel code uses the normal
kernel data selectors for `ds`, `es`, and `fs`; `%gs` is treated as user TLS
state.

This is the key descriptor-table requirement for hosted libc binaries. Static
musl and other Linux i386 runtimes expect TLS setup through
`set_thread_area`, then use `%gs` for thread-local errno and libc internals.

---

## IDT layout

256 gates are installed.  Vectors 0–31 map to the CPU exception stubs
(`isr0`–`isr31`); vectors 32–47 map to the hardware IRQ stubs
(`irq0`–`irq15`) after the 8259 PIC has been remapped (see
[8259 PIC OSDev article](https://wiki.osdev.org/8259_PIC)).

Vector 128 (`0x80`) is the syscall gate: installed with `flags = 0xEE`
(DPL=3) so that user-mode code can invoke it with `int 0x80`.

All other exception and IRQ gates are ring-0-only interrupt gates. Hardware IRQs
are remapped out of the CPU exception range before the IDT is loaded, so IRQ0
starts at vector 32.

---

## Functions

### `init_descriptor_tables`

```c
void init_descriptor_tables(void);
```

Public entry point called from `kernel_main`.  Initialises the GDT (including
the TSS and TLS descriptors), loads it with `gdt_flush` and `tss_flush`,
initialises the IDT (including remapping the 8259 PIC and installing the
syscall gate), loads it with `idt_flush`, then registers the ISR dispatch
table via `init_isr_handlers()`.

### `tss_set_kernel_stack`

```c
void tss_set_kernel_stack(uint32_t esp0);
```

Update the `ESP0` field in the TSS.  Must be called before every ring-3 →
ring-0 transition (i.e. on every task switch) to point the CPU at the correct
per-task kernel stack.

### `gdt_set_tls`

```c
int gdt_set_tls(uint32_t base, uint32_t limit, int limit_in_pages, int present);
```

Program GDT index 6 as a ring-3 data segment and return the index. This is the
descriptor-table side of `SYS_SET_THREAD_AREA`; syscall handling owns ABI
validation and task bookkeeping.

When `present` is false the slot is left not-present, which gives the kernel a
way to represent an inactive TLS descriptor while keeping the selector number
stable. When `limit_in_pages` is true, the descriptor uses 4 KiB granularity;
otherwise it uses byte granularity.

## Invariants

- The kernel code selector is always `0x08`; interrupt gates use it for entry.
- The kernel data selector is always `0x10`; the TSS uses it as `ss0`.
- User code enters with selector `0x1B` and user data with `0x23`.
- User TLS, when active, is selector `0x33`.
- `tss_set_kernel_stack()` must be updated before scheduling a ring-3 task.
- Interrupt and syscall stubs preserve `%gs` so user TLS survives kernel entry.
