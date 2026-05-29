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
#include <kernel/sh_script.h>
#include <kernel/elf.h>

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

/*
 * user_shell_slot_entry – boot the default /apps/sh.elf userspace shell.
 *
 * Normal boot starts one detached userspace shell, mak.sh0.  Additional
 * interactive VT shells belong to the explicit userspace makmux app, not
 * to kernel boot.  This entry registers the initial VT, runs the boot
 * loading/palette prelude, then drops into ring 3 by exec'ing /apps/sh.elf
 * with `--login`.  elf_exec only returns on failure
 * (missing file, malformed ELF, OOM); in that case fall back to the
 * in-kernel rescue shell so the system stays usable.
 *
 * shell=rescue on the kernel cmdline skips this entirely and uses
 * rescu.sh for the case where /apps/sh.elf itself is broken or the rootfs
 * hasn't mounted.
 */
extern void user_shell_slot_entry(void);  /* fwd decl for task_create */
void user_shell_slot_entry(void)
{
	/* mak.sh0 is the detached boot shell.  It gets the loading-screen
	 * tty path, but it is not one of makmux's four switchable VT slots. */
	shell_enter_root_tty();
	{
		task_t *cur = task_current();
		if (cur) {
			cur->name_buf[0] = 'm';
			cur->name_buf[1] = 'a';
			cur->name_buf[2] = 'k';
			cur->name_buf[3] = '.';
			cur->name_buf[4] = 's';
			cur->name_buf[5] = 'h';
			cur->name_buf[6] = '0';
			cur->name_buf[7] = '\0';
			cur->name = cur->name_buf;
		}
	}

	static const char *login_argv[] = { "sh.elf", "--login", NULL };
	int rc = elf_exec("/apps/sh.elf", 2, (const char *const *)login_argv);

	/* elf_exec returned -> /apps/sh.elf could not be loaded.  Print a
	 * diagnostic and fall back to the in-kernel rescue shell on this
	 * VT so the user still has a prompt. */
	Serial_WriteString("user-shell: /apps/sh.elf failed to exec (rc=");
	{
		char dec[12]; int n = 0; int v = rc;
		if (v < 0) { Serial_WriteString("-"); v = -v; }
		if (v == 0) dec[n++] = '0';
		while (v) { dec[n++] = (char)('0' + (v % 10)); v /= 10; }
		while (n--) { char one[2] = { dec[n], 0 }; Serial_WriteString(one); }
	}
	Serial_WriteString("), falling back to kernel rescue shell\n");
	{
		task_t *cur = task_current();
		if (cur)
			cur->name = "rescu.sh";
	}
	shell_run();  /* never returns */
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
	/* Subscribe the display's periodic widgets to the timer rather than
	 * having the timer IRQ reach into the display layer directly. */
	timer_register_tick_hook(t_spinner_tick);
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
	int console_serial = 0;     /* "console=ttyS0" - keep g_serial_verbose
	                             * on after boot so the shell mirrors to
	                             * COM1.  Linux-style: dmesg + tty over
	                             * serial.  Used by ui_test scenarios. */
	const char *root_spec = NULL;   /* `root=...` cmdline arg, NULL = auto */
	static char root_spec_buf[64];  /* copy out of cmdline tag (still alive
	                                 * for the boot, but we own it) */
	int shell_rescue = 0;       /* `shell=rescue` -> use in-kernel rescue
	                             * shell on every VT instead of /apps/sh.elf.
	                             * The recovery path when userspace shell
	                             * or its rootfs is broken. */
	static char test_spec_buf[64];  /* `test=<comma-list>` cmdline arg.
	                                 * Empty = default (ktest + incore +
	                                 * libc-tcc).  Recognised names:
	                                 * "ktest", "incore", "libc-tcc",
	                                 * "all" (= default), "none". */
	const char *test_spec = NULL;
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
					if (strstr(cmd->string, "console=ttyS0"))
						console_serial = 1;
					const char *rp = strstr(cmd->string, "root=");
					if (rp) {
						rp += 5;
						size_t j = 0;
						while (*rp && *rp != ' ' && *rp != '\t' &&
						       j + 1 < sizeof(root_spec_buf)) {
							root_spec_buf[j++] = *rp++;
						}
						root_spec_buf[j] = '\0';
						root_spec = root_spec_buf;
					}
					/* shell=<mode> — currently only "rescue" is
					 * recognised; anything else (or absent) means
					 * normal userspace-shell boot. */
					const char *sp = strstr(cmd->string, "shell=");
					if (sp) {
						sp += 6;
						if (sp[0] == 'r' && sp[1] == 'e' && sp[2] == 's' &&
						    sp[3] == 'c' && sp[4] == 'u' && sp[5] == 'e')
							shell_rescue = 1;
					}
					/* test=<comma-list> -- which test-mode scripts to run. */
					const char *tp = strstr(cmd->string, "test=");
					if (tp) {
						tp += 5;
						size_t j = 0;
						while (*tp && *tp != ' ' && *tp != '\t' &&
						       j + 1 < sizeof(test_spec_buf)) {
							test_spec_buf[j++] = *tp++;
						}
						test_spec_buf[j] = '\0';
						test_spec = test_spec_buf;
					}
				}
				tag_ptr += (tag->size + 7u) & ~7u;
			}
		}

		vfs_set_boot_drive(biosdev);
	}

	vfs_init();
	vfs_mount_root(root_spec);
	vfs_auto_mount();
	vfs_ensure_root_home();

	t_writestring("\nAll subsystems ready.\n");
#ifdef __TINYC__
	t_writestring("Self-hosted kernel! (TCC build)\n\n");
	Serial_WriteString("kernel: build=tcc (self-hosted)\n");
#else
	t_writestring("Host-built kernel (GCC build)\n\n");
	Serial_WriteString("kernel: build=gcc (host-built)\n");
#endif

	t_writestring("Initializing multitasking");
	kprint_ok();
	tasking_init();
	vtty_init();
	if (!test_mode) {
		/* Switch to Linux-style serial behaviour: from here on COM1
		 * only carries explicit kernel diagnostics (KLOG, panic,
		 * driver banners that use Serial_*).  Shell/user TTY output
		 * goes to the framebuffer only, mirroring dmesg semantics.
		 * `console=ttyS0` on the kernel cmdline opts back in so the
		 * shell mirrors to COM1 - used by ui_test scenarios.
		 *
		 * Emit a final boot marker before the flip so external
		 * tooling (CI, debugging shells) can detect "boot complete"
		 * regardless of the mirror policy. */
		Serial_WriteString("kernel: boot complete\n");
		if (!console_serial)
			g_serial_verbose = 0;
		/* shell=rescue boots a single in-kernel rescue shell on VT0
		 * (Linux-style — no other VTs are spawned, so the operator's
		 * keypresses can't be lost to a hung secondary slot).  The
		 * normal path boots one detached /apps/sh.elf login shell as
		 * mak.sh0.  The explicit userspace `makmux` application owns
		 * the multi-VT shell experience. */
		if (shell_rescue) {
			task_create("rescu.sh", shell_run);
		} else {
			task_create("mak.sh0", user_shell_slot_entry);
		}
		task_create("ktest",  ktest_bg_task);
	}

	t_writestring("Initializing syscalls (int 0x80)");
	kprint_ok();
	syscall_init();

	t_writestring("Initializing ACPI");
	kprint_ok();
	acpi_init();

	if (test_mode) {
		/* test=<comma-list> selects which suites run.  Default
		 * (absent / "all") = every suite.  Helpers below treat the
		 * empty-spec case as "all" so a bare `test_mode` cmdline keeps
		 * working unchanged. */
		#define TEST_WANT(name) \
			(test_spec == NULL || test_spec[0] == '\0' || \
			 strcmp(test_spec, "all") == 0 || strstr(test_spec, (name)) != NULL)

		/* Wipe the "Initializing X... [OK]" boot lines off the
		 * framebuffer before the test-mode dispatch starts.  Without
		 * this the test output scrolls on top of the driver init
		 * banner and the visible-window watcher can't tell where the
		 * boot ends and the suite begins.  vesa_tty_clear falls
		 * through to the global pane when no task has a tty (pre-
		 * tasking-init), so this is safe to call here. */
		vesa_tty_clear();
		terminal_initialize();

		int fails = 0;
		if (TEST_WANT("ktest")) {
			fails = ktest_run_all();
			Serial_WriteString(fails ? "KTEST_RESULT: FAIL\n"
			                         : "KTEST_RESULT: PASS\n");
		}

		/* Phase 2: in-kernel UI test driver.  incore.sh exercises
		 * hello / forktest / execvetest / alloctest via exec + $?
		 * checks; marker INCORE: ALL PASS / INCORE: FAIL. */
		if (TEST_WANT("incore")) {
			Serial_WriteString("INCORE: starting\n");
			sh_run_file("/apps/incore.sh");
			Serial_WriteString("INCORE: finished\n");
		}

		/* Non-UI libc + TCC self-rebuild matrix.  Marker
		 * LIBC-TCC: ALL PASS / LIBC-TCC: FAIL. */
		if (TEST_WANT("libc-tcc")) {
			Serial_WriteString("LIBC-TCC: starting\n");
			sh_run_file("/src/userspace/libc-tcc.sh");
			Serial_WriteString("LIBC-TCC: finished\n");
		}

		/* Shell + VFS + apps smoke matrix.  Replaces the HMP
		 * scenarios whose only job was to type a command and grep
		 * serial.  Marker SHELL-SMOKE: ALL PASS / SHELL-SMOKE: FAIL. */
		if (TEST_WANT("shell-smoke")) {
			Serial_WriteString("SHELL-SMOKE: starting\n");
			sh_run_file("/src/userspace/shell-smoke.sh");
			Serial_WriteString("SHELL-SMOKE: finished\n");
		}

		#undef TEST_WANT

		uint8_t exit_val = (fails > 0) ? 1 : 0;
		asm volatile("outb %b0, %w1" :: "a"(exit_val), "Nd"((uint16_t)0xF4));
		for (;;) asm volatile("cli; hlt");
	}

	for (;;) {
		task_yield();
		asm volatile("hlt");
	}
}
