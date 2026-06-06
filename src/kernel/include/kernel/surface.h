#ifndef _KERNEL_SURFACE_H
#define _KERNEL_SURFACE_H

#include <stdint.h>
#include <kernel/task.h>

/*
 * Shared pixel surfaces — kernel-owned blocks of physical frames that can be
 * mapped into more than one task's address space at the same physical
 * location.  This is the one shared-memory primitive Makar has: fork is
 * copy-on-write and SYS_MMAP2 is private-anon only, so a windowed graphical
 * app (e.g. doom.elf) cannot otherwise hand its rendered frames to the
 * compositor (gui.elf).  The window manager creates a surface, passes the id
 * to the forked child, and both map it; the child renders, the WM blits.
 *
 * Lifetime is reference-counted: a surface stays alive while its creator ref
 * is held OR any task still has it mapped.  surface_release_task() is called
 * from the task-teardown path to drop a dying task's mappings (and creator
 * ref) and reclaim the frames once nobody holds them.  Critically it unmaps
 * the surface pages from the dying task's page directory *before*
 * vmm_free_pd() walks it, so the shared frames are never double-freed.
 */

/* 256 pages * 4 KiB = 1 MiB — covers doom's 640x400x4 (~250 pages). */
/* 960 pages * 4 KiB = ~3.75 MiB -- covers a maximised window's surface at the
 * 1280x664 work area (1280*664*4 = ~3.4 MiB = 830 pages) so a windowed client
 * can reallocate its surface up to full-screen on resize. */
#define SURFACE_MAX_PAGES   960
#define SURFACE_MAX          10   /* concurrent surfaces (+headroom: a resize
                                   * briefly holds the old + new surface)     */
#define SURFACE_MAP_SLOTS     4   /* distinct tasks that may map one surface */

/* Create a surface large enough for w*h 32-bpp pixels.  Returns a surface id
 * (>= 0) with a creator reference held, or -1 on bad args / OOM / table full. */
int      surface_create(int w, int h);

/* Map surface `id` into task `t`'s mmap window.  Returns the base user virtual
 * address, or 0 on failure (bad id, no free map slot, address space full). */
uint32_t surface_map(int id, task_t *t);

/* Return (w << 16) | h for `id`, or (uint32_t)-1 if the id is invalid. */
uint32_t surface_info(int id);

/* Drop the creator reference for `id` (only the creator may do this).  Frames
 * are reclaimed once no mappings remain.  Returns 0 on success, -1 on error. */
int      surface_destroy(int id, task_t *caller);

/* Unmap surface `id` from task `t` (clear its PTEs, free the map slot) and
 * reclaim the frames if nothing else references it.  Used when a client
 * reallocates its surface on window resize -- without this each resize would
 * pin the old frames + map slot until the task exits.  Returns 0 on success,
 * -1 on a bad id or if `t` had it unmapped.  Does NOT touch the mmap bump VA. */
int      surface_unmap(int id, task_t *t);

/* Teardown hook: unmap every surface page `t` holds and drop any creator ref
 * it owns, freeing frames that become unreferenced.  Safe to call with t==NULL
 * and on tasks that never touched a surface. */
void     surface_release_task(task_t *t);

#endif /* _KERNEL_SURFACE_H */
