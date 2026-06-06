/*
 * surface.c — kernel-owned shared pixel surfaces.  See kernel/surface.h.
 *
 * A surface is a run of physical frames recorded in a small fixed table.  It
 * can be mapped into several tasks' address spaces at once (same physical
 * frames, independent virtual addresses), giving the window manager and a
 * forked graphical child a shared frame buffer — the only shared-memory
 * mechanism in the system.
 *
 * Reference model: a surface is alive while `creator != NULL` (the creating
 * task still holds it) OR at least one map slot is in use.  When neither holds,
 * its frames are returned to the PMM and the slot is freed.  All mutation runs
 * under a brief cli/sti so a PIT preemption can't interleave the find-free-slot
 * / claim steps (same hazard vtty_register guards against).
 */
#include <kernel/surface.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/serial.h>
#include <string.h>

#define USER_MMAP_BASE   0x90000000u
#define USER_STACK_LIMIT (0xBFFF0000u - (8u * 0x1000u))  /* below ring-3 stack */

typedef struct {
    int       in_use;
    int       w, h, npages;
    uint32_t  phys[SURFACE_MAX_PAGES];
    task_t   *creator;                 /* creator reference, NULL once dropped */
    struct {
        task_t  *task;
        uint32_t base_va;
    } map[SURFACE_MAP_SLOTS];
} surface_t;

static surface_t s_surf[SURFACE_MAX];

/* irq save/restore so the table mutations below are atomic w.r.t. the PIT. */
static inline uint32_t irq_save(void)
{
    uint32_t f;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint32_t f)
{
    __asm__ volatile("pushl %0; popfl" :: "r"(f) : "memory", "cc");
}

/* Reclaim a surface's frames and clear its slot.  Caller holds the irq lock. */
static void surface_gc(surface_t *s)
{
    if (s->creator) return;
    for (int i = 0; i < SURFACE_MAP_SLOTS; i++)
        if (s->map[i].task) return;            /* still mapped somewhere */

    for (int i = 0; i < s->npages; i++)
        if (s->phys[i]) pmm_free_frame(s->phys[i]);
    memset(s, 0, sizeof(*s));
}

int surface_create(int w, int h)
{
    if (w <= 0 || h <= 0) return -1;
    uint32_t bytes  = (uint32_t)w * (uint32_t)h * 4u;
    int      npages = (int)((bytes + 0xFFFu) >> 12);
    if (npages <= 0 || npages > SURFACE_MAX_PAGES) return -1;

    uint32_t flags = irq_save();

    int id = -1;
    for (int i = 0; i < SURFACE_MAX; i++)
        if (!s_surf[i].in_use) { id = i; break; }
    if (id < 0) { irq_restore(flags); return -1; }

    surface_t *s = &s_surf[id];
    memset(s, 0, sizeof(*s));
    s->npages = npages;
    for (int i = 0; i < npages; i++) {
        uint32_t f = pmm_alloc_frame();
        if (f == PMM_ALLOC_ERROR) {
            for (int j = 0; j < i; j++) pmm_free_frame(s->phys[j]);
            memset(s, 0, sizeof(*s));
            irq_restore(flags);
            return -1;
        }
        memset((void *)f, 0, 0x1000u);         /* black/transparent letterbox */
        s->phys[i] = f;
    }
    s->in_use  = 1;
    s->w       = w;
    s->h       = h;
    s->creator = task_current();

    irq_restore(flags);
    return id;
}

uint32_t surface_map(int id, task_t *t)
{
    if (id < 0 || id >= SURFACE_MAX || !t || !t->page_dir) return 0;

    uint32_t flags = irq_save();
    surface_t *s = &s_surf[id];
    if (!s->in_use) { irq_restore(flags); return 0; }

    int slot = -1;
    for (int i = 0; i < SURFACE_MAP_SLOTS; i++)
        if (!s->map[i].task) { slot = i; break; }
    if (slot < 0) { irq_restore(flags); return 0; }

    if (t->mmap_next == 0) t->mmap_next = USER_MMAP_BASE;
    uint32_t base = t->mmap_next;
    if (base + ((uint32_t)s->npages << 12) >= USER_STACK_LIMIT) {
        irq_restore(flags);
        return 0;
    }

    for (int i = 0; i < s->npages; i++)
        vmm_map_page(t->page_dir, base + ((uint32_t)i << 12), s->phys[i],
                     VMM_FLAG_USER | VMM_FLAG_WRITABLE);

    t->mmap_next      = base + ((uint32_t)s->npages << 12);
    s->map[slot].task = t;
    s->map[slot].base_va = base;

    irq_restore(flags);
    return base;
}

uint32_t surface_info(int id)
{
    if (id < 0 || id >= SURFACE_MAX) return (uint32_t)-1;
    surface_t *s = &s_surf[id];
    if (!s->in_use) return (uint32_t)-1;
    return ((uint32_t)(s->w & 0xFFFF) << 16) | (uint32_t)(s->h & 0xFFFF);
}

int surface_destroy(int id, task_t *caller)
{
    if (id < 0 || id >= SURFACE_MAX) return -1;
    uint32_t flags = irq_save();
    surface_t *s = &s_surf[id];
    if (!s->in_use || s->creator != caller) { irq_restore(flags); return -1; }
    s->creator = NULL;
    surface_gc(s);
    irq_restore(flags);
    return 0;
}

int surface_unmap(int id, task_t *t)
{
    if (id < 0 || id >= SURFACE_MAX || !t) return -1;
    uint32_t flags = irq_save();
    surface_t *s = &s_surf[id];
    if (!s->in_use) { irq_restore(flags); return -1; }

    int rc = -1;
    for (int m = 0; m < SURFACE_MAP_SLOTS; m++) {
        if (s->map[m].task != t) continue;
        /* Clear the PTEs first so the frames are no longer reachable from t
         * before we (maybe) free them -- same ordering surface_release_task
         * relies on to avoid a double free. */
        if (t->page_dir)
            for (int p = 0; p < s->npages; p++)
                vmm_unmap_page(t->page_dir,
                               s->map[m].base_va + ((uint32_t)p << 12));
        s->map[m].task = NULL;
        s->map[m].base_va = 0;
        rc = 0;
        break;
    }
    surface_gc(s);
    irq_restore(flags);
    return rc;
}

void surface_release_task(task_t *t)
{
    if (!t) return;
    uint32_t flags = irq_save();
    for (int i = 0; i < SURFACE_MAX; i++) {
        surface_t *s = &s_surf[i];
        if (!s->in_use) continue;

        if (s->creator == t) s->creator = NULL;

        for (int m = 0; m < SURFACE_MAP_SLOTS; m++) {
            if (s->map[m].task != t) continue;
            /* Clear the PTEs in the dying task's PD so vmm_free_pd() won't free
             * the shared frames out from under any other holder. */
            if (t->page_dir)
                for (int p = 0; p < s->npages; p++)
                    vmm_unmap_page(t->page_dir,
                                   s->map[m].base_va + ((uint32_t)p << 12));
            s->map[m].task = NULL;
            s->map[m].base_va = 0;
        }
        surface_gc(s);
    }
    irq_restore(flags);
}
