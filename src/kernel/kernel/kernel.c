#include <stdio.h>
#include <string.h>

#include <kernel/tty.h>
#include <kernel/vga.h>
#include <kernel/descr_tbl.h>
#include <kernel/serial.h>
#include <kernel/timer.h>
#include <kernel/system.h>
#include <kernel/debug.h>
#include <kernel/multiboot.h>
#include <kernel/pmm.h>
#include <kernel/vesa.h>
#include <kernel/vesa_tty.h>
#include <kernel/bochs_vbe.h>
#include <kernel/heap.h>

#include <kernel/paging.h>
#include <kernel/keyboard.h>
#include <kernel/ide.h>
#include <kernel/vfs.h>
#include <kernel/shell.h>
#include <kernel/task.h>
#include <kernel/syscall.h>
#include <kernel/acpi.h>
#include <kernel/ktest.h>
#include <kernel/vtty.h>

/*
 * Column at which "[ OK ]" starts, counting from 0.
 * "[ OK ]" is 6 characters wide, so it occupies columns 74–79 on an
 * 80-column VGA display, flush with the right edge.
 */
#define BOOT_OK_COL (VGA_WIDTH - 6)

/*
 * kprint_ok – stamp a green "[ OK ]" at the right edge of the current line.
 *
 * Call immediately after t_writestring() for the step label and BEFORE the
 * corresponding init function.  This ensures the badge is always on the same
 * row as the step text regardless of any output the init function produces.
 *
 * The badge is written directly into the VGA buffer (and VESA framebuffer if
 * active) at (t_row, BOOT_OK_COL) without moving the cursor, then a newline
 * is emitted so subsequent init output begins on a fresh line.
 */
static void kprint_ok(void)
{
	static const char ok[] = "[ OK ]";
	uint8_t green = make_color(COLOR_LIGHT_GREEN, COLOR_BLACK);

	/* Write directly into the VGA buffer at the fixed column on this row. */
	for (size_t i = 0; i < 6; i++)
		t_putentryat(ok[i], green, BOOT_OK_COL + i, t_row);

	/* Mirror to the VESA framebuffer if it is active. */
	if (vesa_tty_is_ready()) {
		uint32_t vcol = vesa_tty_get_cols() - 6;
		vesa_tty_setcolor(0x00FF00, 0x0000AA);
		for (uint32_t i = 0; i < 6; i++)
			vesa_tty_put_at(ok[i], vcol + i, vesa_tty_get_row());
		vesa_tty_setcolor(0xFFFFFF, 0x0000AA);
	}

	/* Advance the cursor so init output starts on the next line. */
	t_putchar('\n');
}

void kernel_main(uint32_t magic, multiboot2_info_t *mbi)
{
	terminal_initialize();
	t_writestring("Makar kernel starting... (built " __DATE__ " " __TIME__ ")\n");

	t_writestring("Initializing serial COM1");
	kprint_ok();
	init_serial(COM1);
	KLOG("serial: COM1 ready\n");

	t_writestring("Loading descriptor tables");
	kprint_ok();
	init_descriptor_tables();
	KLOG("gdt/idt: descriptor tables loaded\n");

	t_writestring("Installing exception handlers");
	kprint_ok();
	init_debug_handlers();
	KLOG("debug: exception handlers registered\n");

	t_writestring("Initializing physical memory");
	kprint_ok();
	pmm_init(magic, mbi);

	t_writestring("Enabling paging (256 MiB, 4 MiB large pages)");
	kprint_ok();
	paging_init();
	KLOG("paging: init complete\n");

	t_writestring("Initializing heap");
	kprint_ok();
	heap_init();

	t_writestring("Initializing VESA framebuffer");
	kprint_ok();
	vesa_init(mbi);

	t_writestring("Initializing display mode");
	kprint_ok();
	if (bochs_vbe_available()) {
		vesa_tty_set_scale(2);
		bochs_vbe_set_mode(1280, 720, 32);
		vesa_update_geometry(1280, 720, 32);
		vesa_tty_init();
	} else {
		vesa_tty_disable();
		terminal_set_rows(50);
	}

	t_writestring("Starting timer (100 Hz)");
	kprint_ok();
	init_timer(100);
	KLOG("timer: 100 Hz PIT started\n");

	t_writestring("Registering PS/2 keyboard");
	kprint_ok();
	keyboard_init();
	KLOG("keyboard: PS/2 IRQ1 handler registered\n");

	t_writestring("Initializing IDE controller");
	kprint_ok();
	ide_init();
	KLOG("ide: ATA PIO scan complete\n");

	/* Parse Multiboot 2 tags: boot device and kernel command line. */
	int test_mode = 0;
	{
		uint32_t biosdev = 0xFFu;

		if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
			uint8_t *tag_ptr = (uint8_t *)mbi + sizeof(multiboot2_info_t);
			uint8_t *info_end = (uint8_t *)mbi + mbi->total_size;

			while (tag_ptr < info_end) {
				multiboot2_tag_t *tag = (multiboot2_tag_t *)tag_ptr;
				if (tag->type == MULTIBOOT2_TAG_TYPE_END)
					break;
				if (tag->type == MULTIBOOT2_TAG_TYPE_BOOTDEV) {
					multiboot2_tag_bootdev_t *bd =
						(multiboot2_tag_bootdev_t *)tag;
					biosdev = bd->biosdev;
				}
				if (tag->type == MULTIBOOT2_TAG_TYPE_CMDLINE) {
					multiboot2_tag_cmdline_t *cmd =
						(multiboot2_tag_cmdline_t *)tag;
					if (strstr(cmd->string, "test_mode"))
						test_mode = 1;
				}
				tag_ptr += (tag->size + 7u) & ~7u;
			}
		}

		vfs_set_boot_drive(biosdev);
	}

	vfs_init();
	vfs_auto_mount();

	t_writestring("\nAll subsystems ready.\n\n");

	t_writestring("Initializing multitasking");
	kprint_ok();
	tasking_init();
	vtty_init();
	if (!test_mode) {
		task_create("shell0", shell_run);
		task_create("shell1", shell_run);
		task_create("shell2", shell_run);
		task_create("shell3", shell_run);
		task_create("ktest",  ktest_bg_task);
	}

	t_writestring("Initializing syscalls (int 0x80)");
	kprint_ok();
	syscall_init();

	t_writestring("Initializing ACPI");
	kprint_ok();
	acpi_init();

	if (test_mode) {
		int fails = ktest_run_all();
		uint8_t exit_val = (fails > 0) ? 1 : 0;
		Serial_WriteString(exit_val ? "KTEST_RESULT: FAIL\n"
		                            : "KTEST_RESULT: PASS\n");
		asm volatile("outb %b0, %w1" :: "a"(exit_val), "Nd"((uint16_t)0xF4));
		for (;;) asm volatile("cli; hlt");
	}

	for (;;) {
		task_yield();
		asm volatile("hlt");
	}
}
