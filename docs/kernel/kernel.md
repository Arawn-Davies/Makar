# kernel - Boot entry point

**Source:** `kernel/kernel/kernel.c`

This is the top-level kernel module.  It is called directly by the
architecture-specific boot stub (`boot.S`) immediately after the CPU is
placed in protected mode and a valid stack has been set up.

---

## Functions

### `kernel_main`

```c
void kernel_main(uint32_t magic, multiboot2_info_t *mbi);
```

The C entry point of the kernel.  Called from `boot.S` with the Multiboot 2
magic value in `magic` and a pointer to the Multiboot 2 information structure
in `mbi`.

Initialisation order:

1. `terminal_initialize()` - clear the VGA text buffer and set up the cursor.
2. `init_serial(COM1)` - configure the UART at 38 400 baud.
3. `init_descriptor_tables()` - load the GDT and IDT, register ISR stubs.
4. `init_debug_handlers()` - install INT 1 / INT 3 fallback handlers.
5. `pmm_init(magic, mbi)` - build the physical frame bitmap from the Multiboot 2 memory map.
6. `paging_init()` - identity-map the first 8 MiB and enable CR0.PG.
7. `heap_init()` - map the heap region and set up the free-list allocator.
8. `vesa_init(mbi)` - locate the Multiboot 2 framebuffer tag and populate `vesa_fb_t`.
9. `vesa_tty_init()` - map the framebuffer and start the bitmap-font renderer.
10. `init_timer(50)` - program the PIT for 50 Hz and enable interrupts.
11. `kernel_post_boot()` - run the post-boot heartbeat loop.

---

### `kernel_post_boot`

```c
void kernel_post_boot(void);
```

Post-boot heartbeat.  Sleeps for 50 ticks (≈ 1 second at 50 Hz) ten times,
printing the current tick count to both the VGA terminal and the serial port
on each iteration.

This serves two purposes:

- Confirms that the PIT interrupt (IRQ 0) is firing correctly after full
  initialisation.
- Provides named breakpoint targets (`kernel_post_boot`) for the GDB boot
  test suite.

The function is intentionally non-`static` so that GDB can resolve its symbol
by name.
