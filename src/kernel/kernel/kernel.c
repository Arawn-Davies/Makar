#include <stdio.h>
#include <string.h>

#include <kernel/tty.h>
#include <kernel/auth.h>
#include <kernel/vga.h>
#include <kernel/descr_tbl.h>
#include <kernel/fpu.h>
#include <kernel/vm.h>
#include <kernel/serial.h>
#include <kernel/timer.h>
#include <kernel/system.h>
#include <kernel/debug.h>
#include <kernel/multiboot.h>
#include <kernel/pmm.h>
#include <kernel/vesa.h>
#include <kernel/vesa_tty.h>
#include <kernel/bochs_vbe.h>
#include <kernel/video.h>
#include <kernel/heap.h>

#include <kernel/paging.h>
#include <kernel/keyboard.h>
#include <kernel/mouse.h>
#include <kernel/ide.h>
#include <kernel/vfs.h>
#include <kernel/shell.h>
#include <kernel/task.h>
#include <kernel/syscall.h>
#include <kernel/acpi.h>
#include <kernel/pci.h>
void usb_init(void);   /* arch/i386/drivers/usb/usb.h (not on the kernel inc path) */
#include <kernel/net_drivers.h>
#include <kernel/net_lwip.h>
#include <kernel/ktest.h>
#include <kernel/vtty.h>
#include <kernel/sh_script.h>
#include <kernel/elf.h>

/* Set to 1 when `live` appears on the kernel cmdline (live ISO boot).
 * Suppresses login regardless of rootfs type. */
int g_live_boot = 0;

/* `autologin=<user>` from the kernel cmdline.  Overrides /etc/autologin;
 * empty = not set.  Read by shell_login_loop -> auth_try_autologin. */
char g_autologin_user[64] = {0};

/* Set to 1 when `autoboot=gui` is on the kernel cmdline (the GUI boot menu
 * entry).  shell_login_loop launches the desktop in the login session. */
int g_boot_gui = 0;

/* Whether the *current* session is a GUI one (init = g_boot_gui).  Set to 1
 * when the GUI registers as the root-GUI task, 0 on Exit to Shell
 * (SYS_GUI_CLOSE).  shell_login_loop reads it so Log Off re-shows the GUI
 * login but a CLI `logout` re-shows the text login. */
int g_gui_session = 0;

/* `verbose` on the cmdline: skip the boot loading screen/progress bar so the
 * boot log + background ktest output stay visible instead. */
int g_verbose_boot = 0;

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

	/* Linux-style login flow: login_screen → sh.elf session → loop.
	 * shell_login_loop never returns; it shows the login prompt after
	 * every session exit so mak.sh0 always requires re-authentication. */
	shell_login_loop();
}

/*
 * statusbar_entry – become /apps/statusbar.elf, the userspace renderer for
 * the kernel's reserved bottom status row (hostname + clock now; ~/.sbrc
 * widgets later).  Runs as a detached background task with no VT slot; its
 * status-row cells reach the framebuffer regardless of which VT is focused.
 * Waits for the boot ktests so the prompt + enabled row are up first.
 */
extern void statusbar_entry(void);
void statusbar_entry(void)
{
	while (!ktest_bg_done)
		task_yield();
	const char *path = (g_live_boot && vfs_file_exists("/mnt/cdrom/apps/statusbar.elf"))
	                   ? "/mnt/cdrom/apps/statusbar.elf"
	                   : "/apps/statusbar.elf";
	const char *argv[2] = { "statusbar", NULL };
	elf_exec(path, 1, argv);
	/* exec failed (missing/broken ELF): idle harmlessly, no status bar. */
	for (;;)
		task_yield();
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

	t_writestring("Initializing x87 FPU");
	kprint_ok();
	fpu_init();
	KLOG("fpu: x87 armed (fninit) + per-task fxsave/fxrstor on context switch\n");

	/* Hypervisor/VM detection runs after the PCI scan (below) so it can use bus
	 * signals -- e.g. VirtualBox's VMM device -- not just the paravirt-dependent
	 * CPUID leaf.  Nothing before that point consumes vm_kind(); the per-platform
	 * quirks (Hyper-V PS/2 mouse Y) are applied at runtime, well after boot. */

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
	{
		/* vmode=<720p|1080p|480p|WxH> from the boot cmdline.  GRUB and Limine
		 * both feed the multiboot2 CMDLINE tag; the full cmdline parse runs
		 * later, so peek for vmode= now since the display comes up first.
		 * Absent/unsupported -> default 720p. */
		char vmode[16]; vmode[0] = '\0';
		if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
			uint8_t *tp = (uint8_t *)mbi + sizeof(multiboot2_info_t);
			uint8_t *te = (uint8_t *)mbi + mbi->total_size;
			while (tp < te) {
				multiboot2_tag_t *tag = (multiboot2_tag_t *)tp;
				if (tag->type == MULTIBOOT2_TAG_TYPE_END) break;
				if (tag->type == MULTIBOOT2_TAG_TYPE_CMDLINE) {
					const char *vp = strstr(
						((multiboot2_tag_cmdline_t *)tag)->string, "vmode=");
					if (vp) {
						vp += 6;
						size_t k = 0;
						while (*vp && *vp != ' ' && *vp != '\t' &&
						       k + 1 < sizeof(vmode))
							vmode[k++] = *vp++;
						vmode[k] = '\0';
					}
					break;
				}
				tp += (tag->size + 7u) & ~7u;
			}
		}

		if (bochs_vbe_available()) {
			/* Highest mode the adapter can scan out: this is the framebuffer
			 * span we pre-map below, before any task PD is snapshotted, so a
			 * later setmode up to this size never reaches an unmapped FB region
			 * (the 1080p page-fault-at-0xFD400000 bug). */
			/* Highest -> lowest; the first the adapter supports is the max FB
			 * span we pre-map, so any selectable mode up to it is safe to set
			 * later without faulting on an unmapped FB region. */
			static const struct { uint32_t w, h; } prefs[] = {
				{ 1920, 1080 }, { 1600, 900 }, { 1280, 1024 },
				{ 1280, 720 },  { 1024, 768 }, { 640, 480 },
			};
			uint32_t max_w = 0, max_h = 0;
			for (uint32_t i = 0; i < sizeof(prefs)/sizeof(prefs[0]); i++) {
				if (bochs_vbe_mode_supported(prefs[i].w, prefs[i].h, 32)) {
					max_w = prefs[i].w; max_h = prefs[i].h; break;
				}
			}
			if (max_w == 0) { max_w = 1280; max_h = 720; }

			/* Active boot mode: vmode= if given and supported; else default
			 * 720p; else the max the adapter supports.  Accepts named aliases
			 * (480p/720p/900p/1080p) and an explicit "<w>x<h>" (e.g. 1600x900),
			 * so a GRUB/Limine resolution submenu can pass any mode the adapter
			 * advertises -- gated by bochs_vbe_mode_supported() so an unsupported
			 * request (e.g. 1080p on a Hyper-V VBE that only does 1024x768)
			 * cleanly falls back instead of scanning out garbage. */
			uint32_t bw = 0, bh = 0;
			if (vmode[0]) {
				uint32_t rw = 0, rh = 0;
				if      (!strcmp(vmode,"1080p")) { rw=1920; rh=1080; }
				else if (!strcmp(vmode,"900p"))  { rw=1600; rh=900;  }
				else if (!strcmp(vmode,"720p"))  { rw=1280; rh=720;  }
				else if (!strcmp(vmode,"480p"))  { rw=640;  rh=480;  }
				else {                                  /* generic "<w>x<h>" */
					const char *p = vmode; uint32_t v = 0;
					while (*p >= '0' && *p <= '9') v = v*10 + (uint32_t)(*p++ - '0');
					if (*p == 'x' || *p == 'X') {
						rw = v; p++; v = 0;
						while (*p >= '0' && *p <= '9') v = v*10 + (uint32_t)(*p++ - '0');
						rh = v;
					}
				}
				if (rw && bochs_vbe_mode_supported(rw, rh, 32)) { bw = rw; bh = rh; }
			}
			if (bw == 0) {
				if (bochs_vbe_mode_supported(1280, 720, 32)) { bw = 1280; bh = 720; }
				else { bw = max_w; bh = max_h; }
			}

			/* Pre-map the max-supported FB span into the kernel PD (pre-tasking),
			 * write-combining so pixel writes don't trap as UC MMIO on real
			 * VT-x hypervisors / bare metal (see paging_map_region_wc). */
			{
				const vesa_fb_t *fbp = vesa_get_fb();
				if (fbp)
					paging_map_region_wc((uint32_t)(uintptr_t)fbp->addr,
					                     max_w * max_h * 4u);
			}

			{
				uint32_t cw = 0, ch = 0, cb = 0;
				bochs_vbe_caps(&cw, &ch, &cb);
				Serial_WriteString("display: VBE caps max=");
				Serial_WriteDec(cw); Serial_WriteString("x");
				Serial_WriteDec(ch); Serial_WriteString("x"); Serial_WriteDec(cb);
				Serial_WriteString(" vram="); Serial_WriteDec(bochs_vbe_vram_bytes());
				Serial_WriteString(" premap="); Serial_WriteDec(max_w);
				Serial_WriteString("x"); Serial_WriteDec(max_h);
				Serial_WriteString(" -> mode ");
				Serial_WriteDec(bw); Serial_WriteString("x"); Serial_WriteDec(bh);
				if (vmode[0]) { Serial_WriteString(" (vmode="); Serial_WriteString(vmode); Serial_WriteString(")"); }
				Serial_WriteString("\n");
			}

			vesa_tty_set_scale(bw >= 1280 ? 2 : 1);
			bochs_vbe_set_mode(bw, bh, 32);
			vesa_update_geometry(bw, bh, 32);
			vesa_tty_init();
		} else if (vesa_get_fb()) {
			/* No Bochs/DISPI adapter, but the bootloader honoured our
			 * Multiboot2 framebuffer request and handed us a linear
			 * framebuffer (Hyper-V Gen1, VMware SVGA without DISPI, much
			 * real hardware).  The hardware is therefore already in a
			 * GRAPHICS mode -- the VGA text buffer at 0xB8000 is invisible,
			 * which is the Hyper-V Gen1 "black screen" symptom.  We cannot
			 * change the mode (no DISPI registers), so adopt the
			 * bootloader's geometry: map the FB span (write-combining) and
			 * bring vesa_tty up on it. */
			const vesa_fb_t *fbp = vesa_get_fb();
			paging_map_region_wc((uint32_t)(uintptr_t)fbp->addr,
			                     fbp->pitch * fbp->height);
			Serial_WriteString("display: no DISPI; using bootloader LFB ");
			Serial_WriteDec(fbp->width);  Serial_WriteString("x");
			Serial_WriteDec(fbp->height); Serial_WriteString("x");
			Serial_WriteDec(fbp->bpp);    Serial_WriteString("\n");
			vesa_tty_set_scale(fbp->width >= 1280 ? 2 : 1);
			vesa_tty_init();
		} else {
			/* No framebuffer at all: genuine VGA text mode. */
			vesa_disable();
			vesa_tty_disable();
			terminal_set_rows(50);
		}
	}

	t_writestring("Starting timer (250 Hz)");
	kprint_ok();
	init_timer(TIMER_HZ);
	/* Subscribe the display's periodic widgets to the timer rather than
	 * having the timer IRQ reach into the display layer directly. */
	timer_register_tick_hook(t_spinner_tick);
	KLOG("timer: 100 Hz PIT started\n");

	t_writestring("Registering PS/2 keyboard");
	kprint_ok();
	keyboard_init();
	KLOG("keyboard: PS/2 IRQ1 handler registered\n");
	mouse_init();
	KLOG("mouse: PS/2 IRQ12 handler registered\n");

	t_writestring("Initializing IDE controller");
	kprint_ok();
	ide_init();
	KLOG("ide: ATA PIO scan complete\n");

	t_writestring("Scanning PCI bus");
	kprint_ok();
	ide_pci_register();   /* arm bus-master IDE DMA when the controller binds */
	virtio_net_register();
	rtl8139_register();
	e1000_register();
	pcnet_register();
	pci_init();
	pci_probe_all();   /* bind registered drivers to scanned devices */
	KLOG("pci: bus scan complete\n");

	/* Identify the hypervisor/VM now that the PCI bus is enumerated, so the
	 * detection can use bus signals (e.g. VirtualBox's VMM device) alongside
	 * the CPUID hypervisor leaf.  Later drivers read vm_kind() at runtime. */
	vm_detect();
	t_writestring("Virtualization: ");
	t_writestring(vm_name());
	t_putchar('\n');
	KLOG("vm: detected platform\n");

	/* Bind a display driver now that PCI is scanned and the framebuffer
	 * geometry is settled: an accelerated backend (SVGA II / Hyper-V synthvid)
	 * if its hardware is present, else the dumb LFB.  SYS_FB_PRESENT[_RECT]
	 * route through it; nothing presents via the framework before the GUI runs. */
	video_init();
	usb_init();        /* report USB host controllers (HID driver TBD) */

	/* Parse Multiboot 2 tags: boot device and kernel command line. */
	int test_mode = 0;
	int live_boot = 0;          /* `live` on cmdline → live CD session,
	                             * skip login regardless of rootfs type. */
	int kbtest = 0;             /* `kbtest` → spawn the in-guest keyboard
	                             * injection test driver against the live
	                             * shell (deterministic, no host input). */
	int console_serial = 0;     /* "console=ttyS0" - keep g_serial_verbose
	                             * on after boot so the shell mirrors to
	                             * COM1.  Linux-style: dmesg + tty over
	                             * serial.  Used by the in-guest test drivers. */
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
					if (strstr(cmd->string, "verbose"))
						g_verbose_boot = 1;
					if (strstr(cmd->string, "test_mode"))
						test_mode = 1;
					if (strstr(cmd->string, "live"))
						live_boot = 1;
					if (strstr(cmd->string, "kbtest"))
						kbtest = 1;
					if (strstr(cmd->string, "console=ttyS0"))
						console_serial = 1;
					/* `nopreempt` -> legacy serialized syscalls (the
					 * int-0x80 dispatcher keeps IF=0).  Escape hatch if
					 * preemptive syscalls ever misbehave. */
					if (strstr(cmd->string, "nopreempt"))
						g_preempt_enabled = 0;
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
					/* autologin=<user> - skip the password prompt and sign in as
					 * <user> (must exist in /etc/shadow); overrides /etc/autologin. */
					const char *ap = strstr(cmd->string, "autologin=");
					if (ap) {
						ap += 10;
						size_t j = 0;
						while (*ap && *ap != ' ' && *ap != '\t' &&
						       j + 1 < sizeof(g_autologin_user)) {
							g_autologin_user[j++] = *ap++;
						}
						g_autologin_user[j] = '\0';
					}
					/* autoboot=<target> -- auto-start <target> in the login
					 * session.  Only "gui" is recognised (the GUI desktop boot
					 * entry); pairs with autologin=<user> to land on a desktop. */
					const char *bp = strstr(cmd->string, "autoboot=");
					if (bp) {
						bp += 9;
						if (bp[0] == 'g' && bp[1] == 'u' && bp[2] == 'i' &&
						    (bp[3] == '\0' || bp[3] == ' ' || bp[3] == '\t'))
							g_boot_gui = 1;
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
	if (live_boot && !root_spec)
		root_spec = "cdrom";
	vfs_mount_root(root_spec);
	vfs_auto_mount();
	vfs_ensure_root_home();

	t_writestring("\nAll subsystems ready.\n");
	/* MAKAR_BUILD_ORIGIN is set by arch/i386/boot/build_origin.c at
	 * compile time -- "gcc-host" / "tcc-host" / "tcc-in-os". */
	{
		extern const char *MAKAR_BUILD_ORIGIN;
		if (strcmp(MAKAR_BUILD_ORIGIN, "tcc-in-os") == 0) {
			t_writestring("Self-hosted and built inside Makar! (TCC)\n\n");
			Serial_WriteString("kernel: build=tcc-in-os (self-hosted, built inside Makar)\n");
		} else if (strcmp(MAKAR_BUILD_ORIGIN, "tcc-host") == 0) {
			t_writestring("Self-hosted kernel! (TCC, host build)\n\n");
			Serial_WriteString("kernel: build=tcc-host (self-hosted, built on dev host)\n");
		} else {
			t_writestring("Host-built kernel! (GCC)\n\n");
			Serial_WriteString("kernel: build=gcc-host\n");
		}
	}

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
		 * shell mirrors to COM1 - used by the in-guest test drivers.
		 *
		 * Emit a final boot marker before the flip so external
		 * tooling (CI, debugging shells) can detect "boot complete"
		 * regardless of the mirror policy. */
		Serial_WriteString("kernel: boot complete\n");
		if (!console_serial)
			g_serial_verbose = 0;
		g_live_boot = live_boot;
		/* shell=rescue boots a single in-kernel rescue shell on VT0
		 * (Linux-style — no other VTs are spawned, so the operator's
		 * keypresses can't be lost to a hung secondary slot).  The
		 * normal path boots one detached /apps/sh.elf login shell as
		 * mak.sh0.  The explicit userspace `makmux` application owns
		 * the multi-VT shell experience. */
		if (shell_rescue) {
			/* Single-user rescue: come up as root with no login prompt
			 * (physical-console recovery is root-equivalent, like Linux
			 * `init=/bin/sh`).  See auth_force_user / docs. */
			auth_force_user("root");
			task_create("rescu.sh", shell_run);
		} else {
			task_create("net", net_lwip_task);
			task_create("mak.sh0", user_shell_slot_entry);
			/* Userspace status-bar renderer (skipped on the rescue path,
			 * which wants a single bare in-kernel shell). */
			task_create("statusbar", statusbar_entry);
			/* In-guest keyboard injection test driver (cmdline `kbtest`). */
			if (kbtest)
				task_create("kbtest", keyboard_test_driver);
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
		/* Opt-in variant: must be named explicitly.  For long-running suites
		 * we don't want firing on a bare `test_mode` (e.g. the in-OS kernel
		 * rebuild takes minutes in TCG). */
		#define TEST_WANT_EXPLICIT(name) \
			(test_spec != NULL && test_spec[0] != '\0' && \
			 strstr(test_spec, (name)) != NULL)

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

		/* Networking section.  Opt-in (explicit) so the default
		 * `test=ktest` gate stays NIC-agnostic: the net suites need
		 * QEMU slirp + guestfwd and a specific NIC -device.  Run via
		 * `./run.sh nettest [virtio|rtl8139|e1000|pcnet]`. */
		if (TEST_WANT_EXPLICIT("nettest")) {
			int net_fails = ktest_run_net();
			Serial_WriteString(net_fails ? "KTEST_NET_RESULT: FAIL\n"
			                             : "KTEST_NET_RESULT: PASS\n");
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

		/* Shell + VFS + apps smoke matrix: an in-guest sh script whose
		 * per-command $? gates each check.  Marker SHELL-SMOKE: ALL PASS
		 * / SHELL-SMOKE: FAIL. */
		if (TEST_WANT("shell-smoke")) {
			Serial_WriteString("SHELL-SMOKE: starting\n");
			sh_run_file("/src/userspace/shell-smoke.sh");
			Serial_WriteString("SHELL-SMOKE: finished\n");
		}

		/* In-OS kernel rebuild (opt-in only).  Runs /apps/rebuild-kernel.sh
		 * which calls /apps/tcc.elf once per source file then links the
		 * result.  Slow: ~10 min under TCG.  Invoke with:
		 *   TEST_CMDLINE="test_mode test=rebuild-kernel" ./run.sh iso build
		 *   qemu-system-i386 -cdrom makar-test.iso -serial stdio -display none -m 256
		 * Marker REBUILD-KERNEL: ALL PASS / REBUILD-KERNEL: FAIL. */
		if (TEST_WANT_EXPLICIT("rebuild-kernel")) {
			Serial_WriteString("REBUILD-KERNEL: starting\n");
			sh_run_file("/apps/rebuild-kernel.sh");
			Serial_WriteString("REBUILD-KERNEL: finished\n");
		}

		#undef TEST_WANT
		#undef TEST_WANT_EXPLICIT

		uint8_t exit_val = (fails > 0) ? 1 : 0;
		asm volatile("outb %b0, %w1" :: "a"(exit_val), "Nd"((uint16_t)0xF4));
		for (;;) asm volatile("cli; hlt");
	}

	for (;;) {
		task_yield();
		asm volatile("hlt");
	}
}
