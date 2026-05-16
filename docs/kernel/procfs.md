---
title: procfs
parent: Kernel subsystems
---

# procfs - synthetic /proc filesystem

`src/kernel/include/kernel/procfs.h` + `src/kernel/arch/i386/fs/procfs.c`.

## Purpose

Linux-style read-only view of kernel state. Each entry is generated on read
by a small renderer that walks live kernel state and writes ASCII into the
caller's buffer. No on-disk storage; nothing is cached between reads.

## Entries

| Path | Renderer | Content |
|---|---|---|
| `/proc/cpuinfo` | CPUID leaves 0 + 1 | `vendor_id`, `cpu family`, `model`, `stepping`, `flags` (fpu/tsc/msr/pae/apic/cmov/mmx/sse/sse2/sse3/sse4_1/sse4_2), `arch i386 (protected mode)` |
| `/proc/meminfo` | `pmm_free_count()` + `heap_used/free()` | `MemFree`, `PageSize`, `FreeFrames`, `HeapTotal`, `HeapUsed`, `HeapFree` (all in kB where applicable) |
| `/proc/tasks` | Walk `task_pool[]` | One row per task: `PID NAME STATE TTY CWD` |
| `/proc/uname` | `MAKAR_VERSION` + build macros + `timer_get_ticks()` | `Makar 0.5.0 (i386) built <date> <time>` + uptime ticks |

## VFS integration

Wired in [vfs.c](vfs.md) as a new backend (`VFS_FS_PROC = 3`):

- `vfs_route()` recognises any path under `/proc`.
- `vfs_ls("/proc")` calls `procfs_ls()` to print the entry list.
- `vfs_cat()` / `vfs_read_file()` route to `procfs_read_file()`.
- `vfs_complete()` lets tab-completion enumerate `/proc/<entry>`.
- `vfs_file_exists()` returns 1 for known entries.

procfs is **flat** (no subdirectories) and **read-only** (no `vfs_write_file`
path). `cd /proc` is allowed; `cd /proc/cpuinfo` is rejected as not-a-dir.

## Adding a new entry

1. Add a `PROC_XXX` enum value and an entry to the `s_entries[]` table.
2. Write a `render_xxx(pf_writer_t *w)` function using `pf_puts`, `pf_putu`,
   `pf_putc`.
3. Add a dispatch case in `procfs_read_file`.

No changes to vfs.c are needed.
