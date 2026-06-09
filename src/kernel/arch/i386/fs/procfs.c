/*
 * procfs.c - synthetic /proc filesystem.  See kernel/procfs.h.
 */

#include <kernel/procfs.h>
#include <kernel/tty.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/heap.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <kernel/version.h>
#include <kernel/vm.h>      /* vm_name() for the cpuinfo hypervisor line */
#include <kernel/video.h>   /* video_active() -> bound GPU driver name */
#include <kernel/pci.h>     /* identify the actual PCI display device       */
#include <kernel/netdev.h>  /* netdev_present()/netdev_name() -> NIC driver */
#include <kernel/rtc.h>     /* CMOS RTC reader for /proc/rtc */
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Path matching helpers
 *
 * Caller passes a procfs-relative path that always starts with '/'.
 * "/" itself means the procfs root (directory listing).
 * "/<name>" matches a specific entry.
 * ---------------------------------------------------------------------- */

typedef enum {
    PROC_NONE = 0,
    PROC_CPUINFO,
    PROC_MEMINFO,
    PROC_TASKS,
    PROC_UNAME,
    PROC_RTC,
} procfs_id_t;

typedef struct {
    const char  *name;
    procfs_id_t  id;
} procfs_entry_t;

static const procfs_entry_t s_entries[] = {
    { "cpuinfo", PROC_CPUINFO },
    { "meminfo", PROC_MEMINFO },
    { "tasks",   PROC_TASKS   },
    { "uname",   PROC_UNAME   },
    { "rtc",     PROC_RTC     },
    { NULL,      PROC_NONE    },
};

static procfs_id_t classify(const char *path)
{
    if (!path || path[0] != '/') return PROC_NONE;
    if (path[1] == '\0') return PROC_NONE;  /* root, not a file */
    const char *name = path + 1;
    for (const procfs_entry_t *e = s_entries; e->name; e++) {
        if (strcmp(name, e->name) == 0)
            return e->id;
    }
    return PROC_NONE;
}

/* -------------------------------------------------------------------------
 * Buffered writer - a tiny snprintf-style appender that respects the
 * caller's output buffer cap and reports how many bytes it wrote.
 * ---------------------------------------------------------------------- */

typedef struct {
    char    *buf;
    uint32_t cap;
    uint32_t len;
} pf_writer_t;

static void pf_putc(pf_writer_t *w, char c)
{
    if (w->len + 1 < w->cap) {
        w->buf[w->len++] = c;
    }
}

static void pf_puts(pf_writer_t *w, const char *s)
{
    while (*s) pf_putc(w, *s++);
}

static void pf_putu(pf_writer_t *w, uint32_t v)
{
    char tmp[16];
    int  n = 0;
    if (v == 0) { pf_putc(w, '0'); return; }
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) pf_putc(w, tmp[--n]);
}

/* -------------------------------------------------------------------------
 * CPUID
 * ---------------------------------------------------------------------- */

static inline void cpuid_raw(uint32_t leaf,
                             uint32_t *eax, uint32_t *ebx,
                             uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile ("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(leaf));
}

static void render_cpuinfo(pf_writer_t *w)
{
    uint32_t eax, ebx, ecx, edx;

    cpuid_raw(0, &eax, &ebx, &ecx, &edx);
    char vendor[13];
    *(uint32_t *)&vendor[0] = ebx;
    *(uint32_t *)&vendor[4] = edx;
    *(uint32_t *)&vendor[8] = ecx;
    vendor[12] = '\0';
    uint32_t max_leaf = eax;

    pf_puts(w, "vendor_id   : ");
    pf_puts(w, vendor);
    pf_putc(w, '\n');

    /* Processor brand string (CPUID 0x80000002-4): the marketing name like
     * "Intel(R) Core(TM) i5-12400F @ 2.50GHz" or "AMD Ryzen 5 ...".  Reported
     * by most real CPUs and by QEMU; absent on a few very old parts. */
    cpuid_raw(0x80000000u, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000004u) {
        char brand[49];
        uint32_t *bp = (uint32_t *)brand;
        for (uint32_t leaf = 0x80000002u, i = 0; leaf <= 0x80000004u; leaf++) {
            cpuid_raw(leaf, &eax, &ebx, &ecx, &edx);
            bp[i++] = eax; bp[i++] = ebx; bp[i++] = ecx; bp[i++] = edx;
        }
        brand[48] = '\0';
        const char *bs = brand;       /* brand strings are often space-padded */
        while (*bs == ' ') bs++;
        pf_puts(w, "model name  : ");
        pf_puts(w, bs);
        pf_putc(w, '\n');
    }

    if (max_leaf >= 1) {
        cpuid_raw(1, &eax, &ebx, &ecx, &edx);
        uint32_t family   = (eax >> 8)  & 0xF;
        uint32_t model    = (eax >> 4)  & 0xF;
        uint32_t stepping =  eax        & 0xF;
        uint32_t ext_fam  = (eax >> 20) & 0xFF;
        uint32_t ext_mod  = (eax >> 16) & 0xF;
        if (family == 0xF) family += ext_fam;
        if (family == 0x6 || family == 0xF)
            model = (ext_mod << 4) | model;

        pf_puts(w, "cpu family  : "); pf_putu(w, family);   pf_putc(w, '\n');
        pf_puts(w, "model       : "); pf_putu(w, model);    pf_putc(w, '\n');
        pf_puts(w, "stepping    : "); pf_putu(w, stepping); pf_putc(w, '\n');

        pf_puts(w, "flags       :");
        if (edx & (1u << 0))  pf_puts(w, " fpu");
        if (edx & (1u << 4))  pf_puts(w, " tsc");
        if (edx & (1u << 5))  pf_puts(w, " msr");
        if (edx & (1u << 6))  pf_puts(w, " pae");
        if (edx & (1u << 9))  pf_puts(w, " apic");
        if (edx & (1u << 15)) pf_puts(w, " cmov");
        if (edx & (1u << 23)) pf_puts(w, " mmx");
        if (edx & (1u << 25)) pf_puts(w, " sse");
        if (edx & (1u << 26)) pf_puts(w, " sse2");
        if (ecx & (1u << 0))  pf_puts(w, " sse3");
        if (ecx & (1u << 19)) pf_puts(w, " sse4_1");
        if (ecx & (1u << 20)) pf_puts(w, " sse4_2");
        pf_putc(w, '\n');
    }

    pf_puts(w, "arch        : i386 (protected mode)\n");
    pf_puts(w, "hypervisor  : "); pf_puts(w, vm_name()); pf_putc(w, '\n');

    /* Bound device drivers (also surfaced on the GUI About panel). */
    {
        extern const vid_driver_t video_svga2;
        const vid_driver_t *vd = video_active();
        pf_puts(w, "gpu         : ");
        if (vd == &video_svga2) {
            /* The accelerated SVGA II device (PCI 15ad:0405) is the same silicon
             * on every host; report it by each host's product name for it. */
            switch (vm_kind()) {
            case VM_VIRTUALBOX: pf_puts(w, "VMSVGA");        break;
            case VM_VMWARE:     pf_puts(w, "VMware SVGA II"); break;
            default:            pf_puts(w, "VMware SVGA II"); break;  /* QEMU vmware-svga */
            }
            pf_puts(w, " [accelerated]");
        } else {
            /* Dumb-framebuffer path: name the actual PCI display device in use
             * (VirtualBox Graphics Adapter / QEMU Standard VGA / ...), not the
             * generic backend, so the real GPU shows through. */
            const pci_device_t *g = (const pci_device_t *)0;
            for (int i = 0; i < pci_device_count; i++)
                if (pci_devices[i].class_code == 0x03) { g = &pci_devices[i]; break; }
            if (g) {
                const char *vn = pci_vendor_name(g->vendor_id);
                const char *dn = pci_device_name(g->vendor_id, g->device_id);
                if (vn) { pf_puts(w, vn); pf_putc(w, ' '); }
                pf_puts(w, dn ? dn : "display controller");
            } else {
                pf_puts(w, "firmware framebuffer");
            }
            pf_puts(w, " [framebuffer]");
        }
        pf_putc(w, '\n');
    }
    pf_puts(w, "netdev      : ");
    pf_puts(w, netdev_present() ? (netdev_name() ? netdev_name() : "unknown") : "none");
    pf_putc(w, '\n');
}

/* -------------------------------------------------------------------------
 * meminfo
 * ---------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * /proc/rtc -- CMOS real-time clock readout.
 *
 * Decode lives in kernel/drivers/rtc.c so SYS_GETTIMEOFDAY /
 * SYS_CLOCK_GETTIME share the same source.  Format is Linux-style:
 *   YYYY-MM-DD HH:MM:SS
 * --------------------------------------------------------------------------- */

static void render_rtc(pf_writer_t *w)
{
    rtc_time_t t;
    if (rtc_read(&t) != 0) return;

    /* Helper: pad-2 decimal. */
    #define WPAD2(v) do { \
        unsigned _v = (unsigned)(v); \
        pf_putc(w, (char)('0' + (_v / 10u) % 10u)); \
        pf_putc(w, (char)('0' + (_v % 10u))); \
    } while (0)

    pf_putu(w, (uint32_t)t.year);
    pf_putc(w, '-'); WPAD2(t.mon);
    pf_putc(w, '-'); WPAD2(t.day);
    pf_putc(w, ' '); WPAD2(t.hour);
    pf_putc(w, ':'); WPAD2(t.min);
    pf_putc(w, ':'); WPAD2(t.sec);
    pf_putc(w, '\n');

    #undef WPAD2
}

/* /proc/meminfo - Linux-style key/value format ("Label:<spaces>N kB").
 * Field order and naming match Linux for the headline numbers so
 * tools like maktop don't need Makar-specific parsing.  Fields after
 * MemAvailable are Makar-specific kernel-heap diagnostics. */
static void render_meminfo(pf_writer_t *w)
{
    uint32_t total_frames = pmm_managed_count();
    uint32_t free_frames  = pmm_free_count();
    uint32_t used_frames  = (total_frames > free_frames)
                             ? (total_frames - free_frames) : 0u;

    size_t heap_total = (size_t)(0x1800000u - 0x800000u); /* HEAP_MAX-HEAP_START */
    size_t heap_u     = heap_used();
    size_t heap_f     = heap_free();

    /* The kernel image (everything from 0 up to _kernel_end: BIOS low memory
     * + the loaded kernel) and the heap live outside the PMM-managed frame
     * pool, so neither shows up in used_frames.  Count both as used (and add
     * the kernel image into the total) so MemUsed reflects the real working
     * set instead of sitting near zero on a freshly booted 32 MiB system --
     * this is what maktop and the statusbar mem widget display. */
    /* Higher-half kernel: _kernel_phys_end is the physical end of the image. */
    extern uint32_t _kernel_phys_end;
    uint32_t kernel_kb    = (uint32_t)&_kernel_phys_end / 1024u;
    uint32_t heap_used_kb = (uint32_t)(heap_u / 1024u);
    uint32_t total_kb = total_frames * 4u + kernel_kb;
    uint32_t used_kb  = used_frames  * 4u + heap_used_kb + kernel_kb;
    uint32_t free_kb  = (total_kb > used_kb) ? (total_kb - used_kb) : 0u;

    pf_puts(w, "MemTotal:        "); pf_putu(w, total_kb); pf_puts(w, " kB\n");
    pf_puts(w, "MemFree:         "); pf_putu(w, free_kb);  pf_puts(w, " kB\n");
    /* MemAvailable -- Linux's "memory likely usable for new allocations".
     * We don't have page cache to reclaim, so it's the same as MemFree. */
    pf_puts(w, "MemAvailable:    "); pf_putu(w, free_kb);  pf_puts(w, " kB\n");
    pf_puts(w, "MemUsed:         "); pf_putu(w, used_kb);  pf_puts(w, " kB\n");
    pf_puts(w, "Buffers:                0 kB\n");
    pf_puts(w, "Cached:                 0 kB\n");
    pf_puts(w, "HeapTotal:       "); pf_putu(w, (uint32_t)(heap_total / 1024u)); pf_puts(w, " kB\n");
    pf_puts(w, "HeapUsed:        "); pf_putu(w, (uint32_t)(heap_u     / 1024u)); pf_puts(w, " kB\n");
    pf_puts(w, "HeapFree:        "); pf_putu(w, (uint32_t)(heap_f     / 1024u)); pf_puts(w, " kB\n");
    pf_puts(w, "PageSize:               4 kB\n");
    pf_puts(w, "FreeFrames:      "); pf_putu(w, free_frames);  pf_putc(w, '\n');
    pf_puts(w, "TotalFrames:     "); pf_putu(w, total_frames); pf_putc(w, '\n');
}

/* -------------------------------------------------------------------------
 * tasks
 * ---------------------------------------------------------------------- */

static const char *state_name(int s)
{
    switch (s) {
    case 0: return "READY";
    case 1: return "RUN";
    case 2: return "DEAD";
    case 3: return "ZOMB";
    case 4: return "BLOK";
    default: return "?";
    }
}

static void render_tasks(pf_writer_t *w)
{
    pf_puts(w, "PID NAME            STATE TTY    TICKS MEMKB CWD\n");
    int n = task_count();
    for (int i = 0; i < n; i++) {
        task_t *t = task_get(i);
        if (!t) continue;
        /* DEAD slots linger until task_create reclaims them.  Hide them
         * so /proc/tasks shows only live work -- otherwise maktop and
         * `cat /proc/tasks` keep listing finished one-shots like
         * ktest_bg or exec'd userspace binaries indefinitely. */
        if (t->state == TASK_DEAD) continue;
        pf_putu(w, (uint32_t)t->pid);
        pf_putc(w, ' ');
        const char *name = t->name ? t->name : "(noname)";
        uint32_t nl = 0;
        while (name[nl] && nl < 16) { pf_putc(w, name[nl]); nl++; }
        while (nl < 16) { pf_putc(w, ' '); nl++; }
        pf_puts(w, state_name((int)t->state));
        pf_putc(w, ' ');
        if (t->tty < 0) pf_putc(w, '-');
        else            pf_putu(w, (uint32_t)t->tty);
        pf_putc(w, ' ');
        /* CPU time in USER_HZ (100 Hz) units -- stable across TIMER_HZ and
         * unit-matched to SYS_UPTIME so maktop's CPU% ratio is correct. */
        pf_putu(w, timer_to_user_ticks(t->kticks));
        pf_putc(w, ' ');
        /* Per-task memory (KiB): every task owns an 8 KiB kernel stack;
         * ring-3 tasks additionally have their resident user pages.  So
         * shells/idle report their stack baseline and apps report stack +
         * RSS, rather than kernel tasks reading as 0. */
        pf_putu(w, (TASK_STACK_SIZE / 1024u)
                   + vmm_count_user_pages(t->page_dir) * 4u);
        pf_putc(w, ' ');
        pf_puts(w, t->cwd[0] ? t->cwd : "-");
        pf_putc(w, '\n');
    }
}

/* -------------------------------------------------------------------------
 * uname
 * ---------------------------------------------------------------------- */

static void render_uname(pf_writer_t *w)
{
    pf_puts(w, "Makar " MAKAR_VERSION " (i386) built " __DATE__ " " __TIME__ "\n");
    pf_puts(w, "uptime ticks: ");
    pf_putu(w, timer_user_ticks());
    pf_puts(w, " (100 Hz)\n");
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int procfs_file_exists(const char *path)
{
    return classify(path) != PROC_NONE ? 1 : 0;
}

int procfs_read_file(const char *path, void *buf, uint32_t bufsz,
                     uint32_t *out_sz)
{
    procfs_id_t id = classify(path);
    if (id == PROC_NONE) {
        if (out_sz) *out_sz = 0;
        return -1;
    }

    pf_writer_t w = { (char *)buf, bufsz, 0 };
    switch (id) {
    case PROC_CPUINFO: render_cpuinfo(&w); break;
    case PROC_MEMINFO: render_meminfo(&w); break;
    case PROC_TASKS:   render_tasks(&w);   break;
    case PROC_UNAME:   render_uname(&w);   break;
    case PROC_RTC:     render_rtc(&w);     break;
    default: break;
    }

    if (out_sz) *out_sz = w.len;
    return 0;
}

int procfs_ls(const char *path)
{
    if (!path || path[0] != '/') return -1;

    if (path[1] == '\0') {
        for (const procfs_entry_t *e = s_entries; e->name; e++) {
            t_writestring(e->name);
            t_writestring("\n");
        }
        return 0;
    }

    if (classify(path) != PROC_NONE) {
        t_writestring("ls: " PROCFS_MOUNT);
        t_writestring(path);
        t_writestring(": Not a directory\n");
        return -1;
    }
    t_writestring("ls: " PROCFS_MOUNT);
    t_writestring(path);
    t_writestring(": No such entry\n");
    return -1;
}

int procfs_complete(const char *dir, const char *prefix,
                    fat32_complete_cb_t cb, void *ctx)
{
    (void)dir;  /* /proc is flat; the dir argument is always "/proc" */
    if (!cb) return -1;
    size_t plen = prefix ? strlen(prefix) : 0;
    for (const procfs_entry_t *e = s_entries; e->name; e++) {
        if (plen == 0 || strncmp(e->name, prefix, plen) == 0)
            cb(e->name, 0, ctx);
    }
    return 0;
}
