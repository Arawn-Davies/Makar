/*
 * procfs.c - synthetic /proc filesystem.  See kernel/procfs.h.
 */

#include <kernel/procfs.h>
#include <kernel/tty.h>
#include <kernel/pmm.h>
#include <kernel/heap.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <kernel/version.h>
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
}

/* -------------------------------------------------------------------------
 * meminfo
 * ---------------------------------------------------------------------- */

static void render_meminfo(pf_writer_t *w)
{
    uint32_t free_frames = pmm_free_count();
    uint32_t free_kb     = free_frames * 4u;       /* 4 KiB per frame */
    size_t   heap_total  = (size_t)(0x1800000u - 0x800000u); /* HEAP_MAX-HEAP_START */
    size_t   heap_u      = heap_used();
    size_t   heap_f      = heap_free();

    pf_puts(w, "MemFree     : "); pf_putu(w, free_kb);            pf_puts(w, " kB\n");
    pf_puts(w, "PageSize    : 4 kB\n");
    pf_puts(w, "FreeFrames  : "); pf_putu(w, free_frames);        pf_putc(w, '\n');
    pf_puts(w, "HeapTotal   : "); pf_putu(w, (uint32_t)(heap_total / 1024u)); pf_puts(w, " kB\n");
    pf_puts(w, "HeapUsed    : "); pf_putu(w, (uint32_t)(heap_u / 1024u));     pf_puts(w, " kB\n");
    pf_puts(w, "HeapFree    : "); pf_putu(w, (uint32_t)(heap_f / 1024u));     pf_puts(w, " kB\n");
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
    default: return "?";
    }
}

static void render_tasks(pf_writer_t *w)
{
    pf_puts(w, "PID NAME            STATE TTY CWD\n");
    int n = task_count();
    for (int i = 0; i < n; i++) {
        task_t *t = task_get(i);
        if (!t) continue;
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
    pf_putu(w, timer_get_ticks());
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
