---
title: Internals
layout: default
parent: Reference
nav_order: 1
permalink: /internals
---

# Makar internals: a technical walkthrough

This document is for readers who want the *why*. The per-subsystem reference
pages (`docs/kernel/*.md`) explain *what* each module does; this one walks the
machine top-to-bottom — CPU state at boot, paging, TLB management, per-task
address spaces, the scheduler, ring-3 entry, the syscall ABI — and covers
how Makar's POSIX surface (`fork`, `execve`, `wait4`, signals) is actually
implemented today, plus what remains for a broader hosted libc environment.

i386 protected mode, 32-bit, single CPU. No SMP. No PAE. No long mode.
The decisions below are pitched for that target.

---

## 1. CPU state at handoff

GRUB boots us in 32-bit protected mode via the Multiboot 2 protocol. When
`_start` (in `src/kernel/arch/i386/boot/boot.S`) takes control:

| Register / state | Value |
|---|---|
| EAX | `0x36D76289` (Multiboot 2 magic) |
| EBX | physical address of the Multiboot 2 information structure |
| CS  | flat 32-bit code segment installed by GRUB (we replace it) |
| DS/ES/FS/GS/SS | flat data segments (we replace these too) |
| EFLAGS.IF | 0 — interrupts disabled |
| CR0.PG | 0 — **paging is OFF**, addresses are physical |
| CR0.PE | 1 — protected mode is on |
| CR4.PSE | 0 — large pages are off |
| A20 gate | enabled (GRUB handles this) |

`_start` immediately installs a minimal stack (`stack_top`, 16 KiB BSS), saves
EAX/EBX for `kernel_main`, and falls through to C. We're still on GRUB's GDT
at this point; the very first thing `kernel_main` does is reload a GDT we own
(`init_descriptor_tables`). Until that runs, we cannot enter ring 3, cannot
use a TSS, and cannot trust the segment limits — GRUB's segments are flat but
their privilege levels and types aren't our problem to debug.

The kernel image is linked with `link.ld` to load at **1 MiB** (`0x100000`),
which puts it above the BIOS legacy area and BDA. There is no high-half kernel
mapping; the kernel itself runs in the low identity-mapped region. User page
directories still map ring-3 code, stack, heap, and anonymous mmap windows at
their own virtual addresses. The design keeps early boot and kernel debugging
simple while letting ring-3 programs live in a conventional high userspace
layout.

---

## 2. Descriptor tables

### GDT

Seven entries:

| Selector | Index | DPL | Type | Use |
|---|---|---|---|---|
| `0x00`   | 0 | -   | null         | required |
| `0x08`   | 1 | 0   | code, exec/read | kernel CS |
| `0x10`   | 2 | 0   | data, read/write | kernel DS/ES/FS/GS/SS |
| `0x18`   | 3 | 3   | code, exec/read | user CS |
| `0x20`   | 4 | 3   | data, read/write | user DS/ES/FS/GS |
| `0x28`   | 5 | 0   | TSS (32-bit available) | task-state, see below |
| `0x33`   | 6 | 3   | TLS data segment | i386 `set_thread_area`, loaded into `%gs` |

Four things matter here:

- **Flat segmentation.** Every selector covers all of 4 GiB. Paging does all
  the protection work. Segmentation is reduced to "what's the CPL of this
  selector" — exactly the model Linux/NT/most modern PMs use.
- **DPL=3 user segments.** Ring-3 code loads CS=0x1B and SS=0x23 (note the
  RPL bits `0x3`). The `iret` frame in `ring3.S` sets these explicitly.
- **Single TSS.** We don't use hardware task switching — that path is slower
  and uglier than software switching on every x86 since the P6. The TSS
  exists *only* to hold `tss.esp0`, the kernel-stack pointer the CPU loads on
  a privilege-level transition (ring-3 → ring-0 via `int 0x80` or an
  interrupt). Updated by `tss_set_kernel_stack` on every context switch into
  a user task.
- **One TLS slot.** GDT index 6 is the single i386 TLS descriptor used by
  `SYS_SET_THREAD_AREA`. It is reprogrammed for TLS-active tasks, and `%gs`
  is the user TLS selector. The kernel deliberately does not use `%gs`.

### IDT

256 entries, all 32-bit interrupt gates. Generated stubs in `isr_asm.S` push
a fake error code (where the CPU didn't), push the vector number, and jump to
`isr_common_stub` which pushes the full register set, calls into C, and
restores. The stubs reload DS/ES/FS for kernel C code but intentionally leave
`%gs` untouched so a ring-3 task's TLS selector survives syscalls and
interrupts. The interrupt gate type clears IF on entry; we keep it clear for
the duration of the syscall handler (see §10) — a deliberate simplification
that means a syscall can't be preempted, only voluntarily yielded.

DPL on every IDT gate is **zero**, *except* the syscall gate at 0x80, which is
DPL=3. Ring-3 cannot raise an arbitrary interrupt; the only doorbell it has
is `int 0x80` (and `int3` debug, which we also expose).

---

## 3. Physical memory manager

`src/kernel/arch/i386/mm/pmm.c`. A bitmap allocator, one bit per 4 KiB frame,
indexed from physical address 0. Bootstrap is the tricky part:

1. Walk the Multiboot 2 memory-map tag. Each `mmap_entry` describes a
   contiguous physical range and its type (available, reserved, ACPI
   reclaimable, etc.). We track only `MULTIBOOT_MEMORY_AVAILABLE` ranges.
2. Compute the total managed frames, allocate the bitmap *inside* one of the
   available ranges (bumped past the kernel image so we don't clobber
   ourselves), and mark the bitmap's own footprint as used.
3. Mark every non-available range as used (so frame allocation never returns
   a frame we don't actually own — ACPI tables, MMIO holes, the EBDA).
4. Mark frame 0 as used. We never return the null page. This is what makes
   `vmm_map_page(pd, 0x00000000, ...)` a guaranteed-NULL deref rather than
   accidentally mapping the IVT.

`pmm_alloc_frame` is a linear scan from the last-allocated index, wrapping
on the way back. That's not O(1), but it's O(bits/64) with `bsf`-friendly
access patterns, and the bitmap fits in 4 KiB even for 128 MiB of RAM, so
this is well below the noise floor on every hot path that calls it.

`PMM_ALLOC_ERROR` is `(uint32_t)-1`. Callers check it; the kernel doesn't
panic on OOM at this layer because the allocator can be called from contexts
where panicking is worse than gracefully failing (e.g., a faulting user-page
allocator that can return a signal instead).

---

## 4. Paging

`src/kernel/arch/i386/mm/paging.c`. The kernel installs **one** page
directory at `paging_kernel_pd`. The first 64 PDEs are populated with 4 MiB
PSE large-page entries (`PS=1, PRESENT=1, RW=1`) mapping virtual `0` →
physical `0` through virtual `0x10000000` → physical `0x10000000`. That gives
us a flat **256 MiB identity window** that the kernel runs in.

`paging_init` does the actual mode flip in this order, which matters:

```c
/* 1. CR4.PSE: tell the CPU PDEs with PS=1 are 4 MiB pages.
 *    Must precede CR0.PG=1 or the first faulted PDE walk will treat
 *    PS=1 as "this is a normal PDE pointing at PT 0x800000". */
cr4 |= (1u << 4);
asm volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

/* 2. Load CR3 with the kernel PD's physical address.  This populates
 *    the TLB tagger but doesn't actually map anything until PG=1. */
asm volatile("mov %0, %%cr3" :: "r"(&pd) : "memory");

/* 3. CR0.PG: turn paging on.  Immediately after this instruction
 *    retires, every linear address is page-translated.  The kernel
 *    is identity-mapped so EIP stays valid. */
cr0 |= (1u << 31);
asm volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
```

Three things worth a lecture call-out:

- **PSE before PG.** If you flip PG before PSE, the CPU walks the PDE with
  `PS=1` and treats bits `[31:12]` as a page-table base address, which is
  catastrophic. Some emulators (QEMU TCG) tolerate the wrong order; real
  hardware does not.
- **CR3 reload is itself a full TLB flush** (except for entries tagged with
  the global bit, which we don't use). So the moment PG goes on, the TLB is
  cold; the next instruction fetch fills one 4 MiB TLB entry for the EIP
  page. No `invlpg` needed.
- **No global pages.** We never set the G bit on kernel PDEs. The right thing
  long-term is `CR4.PGE=1` + `G=1` on the kernel identity window so a CR3
  reload during context switch doesn't flush the kernel TLB. Today every
  `vmm_switch` re-fills the kernel TLB entries on first use. Cheap on a
  hobby OS (256 MiB / 4 MiB = 64 TLB fills), worth fixing on the day we
  benchmark scheduler throughput.

### Page-fault delivery

Vector 14. The hardware pushes the faulting linear address into CR2 and a
4-bit error code onto the stack:

```
bit 0: P     1 if a protection violation (rather than not-present)
bit 1: W/R   1 on write, 0 on read
bit 2: U/S   1 if from ring 3
bit 3: RSVD  1 if a reserved-bit was set in a page-table entry
bit 4: I/D   1 on an instruction fetch (PAE/long mode only)
```

The handler in `debug/debug.c` (`page_fault_handler`) resolves three cases
before giving up, in order:

1. **Demand paging** (`try_handle_anon_fault`) — a *not-present* fault inside the
   faulting task's reserved heap `[user_brk_base, user_brk)` or anonymous-mmap
   `[USER_MMAP_BASE, mmap_next)` window maps a fresh zeroed frame and retries.
   `SYS_BRK`/`SYS_MMAP2` only reserve the range (Linux's lazy model), so a
   multi-MiB allocation costs O(1) in the syscall and the per-page work is spread
   across short faults instead of stalling the system in one interrupts-off loop.
2. **Copy-on-write** (`try_handle_cow_fault`) — a *write* to a present RO+COW page
   (fork) copies or re-promotes it (§11).
3. Otherwise: a ring-3 fault delivers `SIGSEGV` (the offender is reaped); a ring-0
   fault is a real kernel bug and panics.

---

## 5. Per-task address spaces (VMM)

`src/kernel/arch/i386/mm/vmm.c`. Every task created with `task_create_user`
gets its own page directory. Construction (`vmm_create_pd`):

1. `pmm_alloc_frame` for the PD itself.
2. `memset` to zero, then walk the kernel PD and copy every present PDE
   (indices 0–63 — the 256 MiB identity window plus anything `heap_init` or
   `vesa_init` added). This is the **kernel-PDE-mirroring** model — every
   user PD has the kernel mapped at the same virtual addresses as the kernel
   sees itself.
3. Leave indices 64–1023 zero. That's the user-space range, 3.75 GiB worth,
   though we only ever populate two slots:
   - `USER_CODE_BASE = 0x40000000` (PDE 256) — one 4 KiB page for code.
   - `USER_STACK_TOP = 0xBFFF0000` (PDE 767) — `USER_STACK_PAGES = 8` pages (32 KiB) eagerly mapped at exec, occupying `[USER_STACK_TOP − 32 KiB, USER_STACK_TOP)`.  Was one 4 KiB page until TCC's recursive-descent parser overflowed it on sh.c.
   - **Heap** (`brk`, grows up from the end of the image) and the **anonymous
     mmap window** (`USER_MMAP_BASE = 0x90000000`, PDE 576) are *demand-paged*:
     `SYS_BRK`/`SYS_MMAP2` only advance the reservation pointer, and the
     page-fault handler maps zeroed frames on first touch (§4). This is what
     keeps a big `malloc` (e.g. DOOM's 6 MiB zone) from mapping ~1500 pages in
     one interrupts-off syscall.

The mirror is a one-time snapshot, not a live shadow. If something maps a new
kernel PDE *after* `vmm_create_pd` runs (e.g., the heap grows past 16 MiB), the
existing user PDs miss it. The heap is pre-mapped at boot to its full 16 MiB
window precisely to dodge this. The proper fix is the global-page approach
above, or a small "kernel PDE update" propagation routine.

### Mapping a page

`vmm_map_page` is the only routine that allocates **4 KiB page tables**. If
the target PDE is unpopulated, it allocates a frame, zeros it, installs it as
the PT with `PRESENT|WRITABLE|USER` (note: the PDE flags must permit user
access if any single PTE in it does; the CPU walks both). Then it sets the
PTE with the caller's flags.

The user range deliberately has `PAGE_USER` on PDEs so ring-3 can walk in.
The kernel-mirrored PDEs use 4 MiB pages without `PAGE_USER`, which makes
*any* ring-3 attempt to read or write a kernel address trap with U/S=1 in the
PFE — clean privilege separation enforced by hardware, not software.

### TLB management

This is the section your manager will ask the most pointed questions about.

- **`vmm_switch(pd)`** writes CR3, which on every x86 since the i486 flushes
  the entire non-global TLB. That's what we have on every context switch
  (`schedule()` calls it whenever the destination task has a different PD
  than the source). On a 100 Hz scheduler with 8 tasks, that's at worst a
  few hundred flushes/second — fine.
- **`vmm_unmap_page`** invokes `invlpg` *only if the current CR3 matches the
  PD being mutated*. Otherwise the stale entry lives in whichever
  hypothetical TLB belongs to the other-task's CR3 (which doesn't exist on
  this CPU — TLBs are per-physical-CPU, and CR3 reload is the
  scaling-equivalent on uniprocessor). When we switch back to that PD, CR3
  reload flushes it. Net: correct on uniprocessor, *broken* on SMP without
  a proper TLB-shootdown IPI. We are not SMP.
- **`vmm_map_page`** does **no** TLB invalidation. The mapped page wasn't
  present before, so the TLB couldn't have a cached translation for it.
  This is the standard "lazy mapping" path and it's safe by construction —
  the only failure mode is if you `unmap` then immediately `map` the same
  vaddr; we handle that explicitly elsewhere.

### Why 4 MiB pages for the kernel

Because they cost one TLB slot per 4 MiB instead of 1024 slots for the same
range. The full kernel identity window (256 MiB) fits in **64** TLB entries,
and most x86 CPUs since the P6 have a split TLB with a dedicated 4 MiB
section large enough to hold the whole kernel map indefinitely. Combined
with no global bit (next bullet), this is the dominant reason context-switch
overhead on Makar is dominated by `mov %cr3` itself (a ~100-cycle
serialising instruction), not by re-faulting kernel TLB entries.

### Why the heap maps eagerly

`heap_init` calls `paging_map_region(HEAP_START, HEAP_MAX - HEAP_START)` —
16 MiB worth — at boot. That eagerly installs the PT entries in the kernel
PD before any user PD is cloned, so the mirror in `vmm_create_pd` catches
them all. A lazier heap (map on first touch) would force us to either
propagate kernel-PDE updates on every fault or use global pages. Pick your
poison; on a 128-MiB-RAM hobby OS, the 16 MiB eager allocation is the
cheaper one to spell out.

---

## 6. Kernel heap

`src/kernel/arch/i386/mm/heap.c`. First-fit linked-list allocator over a
single 16 MiB region. Each block has a header:

```c
typedef struct block_hdr {
    uint32_t size;      /* payload size, NOT including header */
    uint32_t free : 1;
    uint32_t      : 31;
} block_hdr_t;
```

`kmalloc(n)` walks from `heap_head`, picks the first free block whose
`size >= n`, splits if the leftover would hold at least a header + 16 bytes,
and returns the payload pointer (`block + 1`). `kfree(p)` flips the free bit
and coalesces forward (rightward) — it does **not** coalesce backward,
because the list has no `prev` pointer. This is a known small-O fragmentation
hazard; if it ever bites we add a `prev` field and pay the 4 extra bytes per
header.

No alignment guarantees beyond 4 bytes. Not a buddy allocator, not a slab
allocator. Sufficient for a kernel where the hot allocations are a handful
of `task_t` and `vesa_pane_t` and a few KiB of FAT32 buffers.

---

## 7. Tasking

`src/kernel/arch/i386/proc/task.c`. Fixed-size pool of `MAX_TASKS` (32) `task_t`
slots, round-robin scheduler, voluntary `task_yield()` *and* preemptive
timer-driven yields (PIT `TIMER_HZ` = 250 Hz, `g_sched_quantum = 1` tick → 4 ms
time slice).

Ring-3 is preemptible (the timer yields between slices); **syscalls are still
serialized** — the `int 0x80` gate clears IF and the handler keeps it clear, so
a syscall runs to completion or yields voluntarily (§10). Long operations opt
into preemption explicitly instead (`sti` inside the installer's stepped copy),
and the worst eager stalls were removed structurally (demand-paged `brk`/`mmap`,
§4). The shared mutable structures a future global `sti` would expose —
`schedule()`, `task_exit`, the kernel heap, the task pool (`task_create`/
`task_fork`) and the PMM buddy lists — are already `cli`-guarded so flipping it
is gateable; the remaining prerequisite is an lwIP net lock plus fork/exec
stress testing.

### `task_t` (abbreviated)

```c
typedef struct task {
    uint32_t  pid;
    char     *name;
    char      name_buf[TASK_NAME_MAX];

    /* Scheduler state */
    enum task_state state;     /* RUNNABLE / RUNNING / DEAD */
    uint32_t        esp;       /* saved kernel-stack pointer */
    uint8_t        *stack;     /* kernel stack base */

    /* Address space (NULL for kernel-only tasks) */
    uint32_t       *page_dir;
    uint32_t        user_brk;

    /* TTY / fd / signals */
    int             tty;
    fd_table_t     *fd_table;
    sig_handler_t   sig_handlers[NSIG];
    uint32_t        sig_pending, sig_mask;

    /* Cwd, exec params, fb_touched, kticks, unkillable, ... */

    struct task *next;
} task_t;
```

### Context switch (`task_asm.S`)

```
void task_switch(uint32_t *old_esp, uint32_t new_esp);
```

The asm pushes EDI, ESI, EBX, EBP and pushfd onto the *current* stack, writes
the resulting ESP into `*old_esp`, loads ESP from `new_esp`, popfd, and pops
the four callee-saved regs. Then `ret` — which on the new task pops the
return address the *previous* call to `task_switch` left there. Net effect:
control returns into the function that called `task_switch` last time, on the
new task's stack, with that task's EFLAGS (and therefore its IF) restored.

We save *only* callee-saved registers because the caller (always the C
`schedule()` function) has already spilled what the ABI doesn't require us to
preserve. This is the same trick Linux uses for its `__switch_to` family.

### Preemption + the `in_schedule` guard

`timer_callback` calls `schedule()` from IRQ 0 context. `schedule()` can also
be entered cooperatively via `task_yield`. Both paths now go through a
re-entrancy guard:

```c
static volatile int in_schedule = 0;

static inline uint32_t irq_save_disable(void) {
    uint32_t f; asm volatile("pushfd; popl %0; cli" : "=r"(f));
    return f;
}

static void schedule(void) {
    uint32_t saved = irq_save_disable();
    if (in_schedule) { irq_restore(saved); return; }
    in_schedule = 1;
    ...
    in_schedule = 0;
    irq_restore(saved);
}
```

Why: without the guard, a preemptive timer tick that arrives *while* a
cooperatively-yielded `schedule()` is mid-list-walk would corrupt the
runqueue. The `cli` is the standard one-CPU mutex; `in_schedule` is the
re-entrancy bit that turns nested calls into no-ops instead of deadlocks.
The `irq_save_disable` form is important — we re-enable IF only if the
caller had it on, so syscalls (which keep IF=0 for the duration) don't have
their flag flipped under them.

### Reaper

`task_exit` flips state to `TASK_ZOMBIE` (if the parent is a live ring-3
task that might `wait4`) or `TASK_DEAD` (otherwise — kernel-internal tasks,
or orphans whose parent has died) and yields.  See §11.3 for the lifecycle
state machine.  Either way the next `schedule()` that runs on a different
PD frees the dead task's PD via `pmm_free_frame` (plus the heap-allocated
PT pages it owns). We deliberately don't free the PD inline because the
current CR3 may *be* that PD; the CR3 switch in `vmm_switch` must happen
before we hand the frame back to the PMM. This is exactly the "delayed
reaper" pattern Linux uses for `free_task_struct`.

A `TASK_ZOMBIE` task continues to occupy its pool slot (preserving
`exit_status` for the parent's `wait4`) but has its PD freed by the
schedule-time reaper exactly the same way; only when the parent reaps it
does the slot transition to `DEAD` and become reclaimable by `task_create`.

---

## 8. Ring-3 entry

`src/kernel/arch/i386/proc/ring3.S`. `ring3_enter(entry, stack_top)` builds
a 5-word `iret` frame:

```
[ESP+16]  SS    = 0x23   (user data, RPL=3)
[ESP+12]  ESP   = stack_top
[ESP+ 8]  EFLAGS = saved | IF  (we want IF=1 in ring 3)
[ESP+ 4]  CS    = 0x1B   (user code, RPL=3)
[ESP+ 0]  EIP   = entry
```

Loads DS/ES/FS/GS with `0x23`, then `iret`. Hardware unwinds CS:EIP/SS:ESP
*and* sets CPL from CS's RPL, in a single atomic step. Returns to ring 3
without ever existing in an intermediate "still in ring 0 but with user
segments" state. This is the entire reason `iret` is used for ring transitions
rather than a `jmp far` sequence — it's the only way to flip CPL atomically.

The caller's responsibility before `ring3_enter`:

1. `tss_set_kernel_stack(top_of_kernel_stack)` — so the *next* `int 0x80`
   knows where to set ESP after the privilege flip.
2. `vmm_switch(pd)` — load the user PD into CR3.

ELF loading (`elf_exec`) maps `USER_CODE_BASE` to a freshly-allocated frame,
copies the program text in, maps `USER_STACK_PAGES` (8 × 4 KiB = 32 KiB) of
user stack below `USER_STACK_TOP`, constructs argc/argv on the top page, and
jumps through `ring3_enter`. We have no dynamic loader; binaries are
statically linked freestanding ELFs.

---

## 9. Syscall ABI

`int 0x80`, Linux i386 convention. EAX = syscall number; EBX/ECX/EDX/ESI/EDI
= arg0..arg4. Return value comes back in EAX.

For a full table of which POSIX syscalls Makar implements vs. omits
(plus the Makar-200+ extensions that fill in for the absent `ioctl` /
`termios` / `clock_gettime` plumbing), see
[`docs/posix.md`](posix.md).

```
ring 3                                          ring 0
  ─────                                          ─────
  mov eax, SYS_WRITE                           
  mov ebx, fd                                  
  mov ecx, buf                                 
  mov edx, len                                 
  int 0x80      ──────────────────►            isr_common_stub
                                                pushes regs, calls C
                                                                  │
                                                                  ▼
                                                syscall_handler(regs)
                                                  - dispatch on regs->eax
                                                  - write result to regs->eax
                                                                  │
                ◄──────────────────             iret w/ saved EFLAGS
  result in EAX                                  (IF restored to user 1)
```

The trap gate at IDT[0x80] is DPL=3 so ring 3 can fire it. Inside the
handler, IF stays 0 — interrupts are masked for the syscall's duration.
This is the simplest thread-safety story: a syscall can't be preempted, so
no syscall handler needs to be reentrant or take locks against IRQ context.
The cost is latency — a slow syscall (FAT32 read, IDE access) holds the IRQ
mask for milliseconds. Disk transfers now prefer bus-master DMA
(`ide.c`, BMIDE BAR4) over the old per-word PIO loop, which both shortens
that window and — more importantly — avoids ~256 VM exits per sector on VT-x
hypervisors (VirtualBox, Hyper-V); PIO remains the fallback when no DMA-capable
controller is found. Acceptable on a single-task interactive shell; the
fix is to *enable* IF inside the handler once we're past the regs-save and
no longer running on a possibly-corrupt stack, the same pattern Linux uses
for `local_irq_enable()` inside its syscall path. The plumbing is there
(`irq_save_disable` etc.); we just haven't pulled the trigger.

The full table lives at `src/kernel/include/kernel/syscall.h`. Numbers 1..49
match Linux/i386 exactly (EXIT, READ, WRITE, OPEN, CLOSE, LSEEK, KILL, BRK,
SIGNAL, FCNTL...). 100, 158, 200–218 are Makar-only extensions for terminal
ops, signal returns, the pixel framebuffer API (`SYS_DRAW_LINE` 217,
`SYS_CARET_STYLE` 218), and `SYS_GETCWD`. `OPEN`/`READ`/`WRITE`/`LSEEK`
also drive `/dev` block devices: a `/dev` node opens as `FD_KIND_BLOCKDEV`
(no eager buffer) and read/write/seek do byte-addressed sector I/O.

#### Lazy read-only files + the page cache

A `read-only` open on a seekable disk backend (ext2/fat32/iso9660) is **not**
eager-loaded. The fd records only `{path, size, pos}` (`fd_entry_t.lazy`); each
`SYS_READ` streams the needed bytes through the kernel **page cache**
(`mm/pagecache.c`) — a fixed pool of 4 KiB pages in an LRU list, keyed by
`{path-hash, page index}`, filled on a miss via `vfs_read_at()` →
per-backend `*_read_at` (iso9660 contiguous extent; ext2 block-map walk; fat32
cluster-chain walk). So opening a ~29 MiB FreeDOOM WAD costs ~0 heap and DOOM
streams lumps like it does on DOS, instead of the old whole-file `kmalloc` (the
band-aid that pushed `SYSCALL_FILE_MAX` to 32 MiB and interactive boots to
`-m 256`, both now reverted). Writable opens and synthetic backends
(procfs/tmpfs/...) keep the buffered path. Correctness: the cache holds clean
read-only data only; the VFS write/delete/rename paths call
`pagecache_invalidate(path)` (outside the disk big-lock) so a later read never
sees stale bytes — verified by the libc-tcc gate (compile → write `.o` →
`ar` → link → run). The cache is a fixed static pool (bounded, self-evicting);
a dynamic, PMM-pressure-driven shrinker is a documented follow-up.

### `int 0x80` vs `sysenter`

Modern Linux on i686 uses `sysenter` (CSE-enabled fast syscalls) when the CPU
supports it, falling back to `int 0x80` on ancient hardware. We use `int 0x80`
exclusively. Pros: simple. Cons: ~80–120 cycles latency vs ~20 cycles for
`sysenter`. On a hobby OS where the hot path is a shell at 1 syscall/second
on average, this doesn't move the needle. The day we run a real workload it
becomes a 2-day port (set up MSR_IA32_SYSENTER_*, write a sysenter entry stub,
flip the libc to use it).

---

## 10. Interrupts

Single 8259A pair, remapped from BIOS-default (IRQs at vectors 8–15, conflict
with exceptions) to vectors **32–47**. PIT on IRQ 0 = vector 32; PS/2
keyboard on IRQ 1 = vector 33; IDE on IRQ 14 = vector 46.

The remap dance (`pic_remap`):

```c
outb(PIC1_CMD, 0x11);   outb(PIC2_CMD, 0x11);   /* init, expect ICW2-4 */
outb(PIC1_DATA, 0x20);  outb(PIC2_DATA, 0x28);  /* offsets 32 / 40    */
outb(PIC1_DATA, 0x04);  outb(PIC2_DATA, 0x02);  /* cascade IRQ 2      */
outb(PIC1_DATA, 0x01);  outb(PIC2_DATA, 0x01);  /* 8086 mode          */
outb(PIC1_DATA, 0x00);  outb(PIC2_DATA, 0x00);  /* mask = none        */
```

After each IRQ handler returns, the dispatcher writes `0x20` (EOI) to the
appropriate PIC. The slave-PIC IRQs (8..15) require EOI to both. The kernel
never enters this path with IF=1 — IDT gates clear IF on entry and `iret`
restores the user's IF on return.

We do not use the APIC. We do not use the HPET. The PIT at 100 Hz drives
both the scheduler and `sys_uptime()`. Adopting the APIC would buy us
per-CPU timers (irrelevant pre-SMP) and per-IRQ programmable priorities
(useful for IDE-vs-keyboard contention, but not enough to justify the port).

---

## 11. fork(), execve(), wait4() — how they actually work

This section used to be speculative ("the road to fork()"); slices 15 and 16
shipped the lot.  What follows documents how each piece is implemented today,
plus what's still missing for a musl/dash port.

### 11.1 `fork()` via copy-on-write (slice 15)

POSIX `fork()` creates a child that's a logical copy of the parent: same
contents, independent writes.  The textbook implementation is **copy-on-write**
— parent and child share physical frames marked read-only, and a `#PF` handler
clones the frame on the first write.  That's what Makar does.

**Per-frame refcounts** (slice 15a, `arch/i386/mm/pmm.c`).  The PMM bitmap
gained a parallel `uint8_t refcount[PMM_MAX_FRAMES]`.  `pmm_alloc_frame` sets
refcount=1; `pmm_free_frame` decrements and only releases the bitmap bit when
the count hits zero; new `pmm_inc_ref` / `pmm_ref_count` complete the API.
All pre-existing single-owner callers (heap, vmm) keep their behaviour for
free — they alloc → free with the refcount cycling 0→1→0 just as before.

**`vmm_clone_pd_cow()`** (slice 15b, `arch/i386/mm/vmm.c`).  Walks the parent
PD; for each present user PTE: bumps the frame refcount, clears
`PAGE_WRITABLE`, sets a software COW bit (`VMM_PTE_COW` = PTE bit 9 —
hardware-ignored, one of the three OS-available bits), mirrors the resulting
PTE into a freshly-allocated child PT.  Kernel PDEs (shared with `kpd[]`,
identity-mapped) are passed through as-is.  Reloads CR3 if the parent is
currently active so the freshly-RO PTEs take effect immediately (otherwise
the next parent write would silently succeed against a cached writable TLB
entry).

**COW `#PF` handler + `CR0.WP`** (slice 15c, `arch/i386/debug/debug.c`).  The
page-fault handler now tries `try_handle_cow_fault` before falling through to
the panic screen:

```
write fault?  page present?  PTE has VMM_PTE_COW set?
        ↓ all yes
   pmm_ref_count(frame) <= 1
        ├── yes → sole owner; just clear COW + set RW, invlpg
        └── no  → alloc fresh frame, memcpy 4 KiB, pmm_free_frame(old),
                  install fresh frame in this task's PTE as RW, invlpg
```

`CR0.WP` is enabled at `paging_init` so kernel writes to user RO pages also
fault — required to make COW work uniformly when a syscall reads from / writes
to a parent-shared user buffer (e.g., `SYS_READ` filling a buffer the child
inherited).  Linux and ELKS both do this for the same reason.

**`SYS_FORK` (= 2)** (slice 15d, `arch/i386/proc/task.c` + `task_asm.S`).
`task_fork()` clones a task pool slot, deep-copies the fd_table via
`fd_table_clone` (FILE-kind slots get their own kmalloc'd buffers — non-POSIX
shared-seek semantics, deferred to a refcounted `open_file_t` later), inherits
cwd/tty/user_brk, clones the PD via `vmm_clone_pd_cow`, and resets the
per-task signal handler table.  Then it hand-builds the child's kernel stack:

```
[stack_top high addr]
registers_t          ← copy of parent's at int 0x80 entry, EAX patched to 0
fork_child_iret      ← task_switch ret target
EFLAGS = 0x002       ← popf (IF=0 in kernel mode; user EFLAGS from iret frame)
ebp / ebx / esi / edi ← all zero
[t->esp]
```

`fork_child_iret` (in `task_asm.S`) is a 1:1 mirror of the
`isr_common_stub` epilogue: pop ds + set data segments, popa, addl over
`err_code + int_no`, iret to ring 3.  When the scheduler picks the child for
the first time, `task_switch` pops the callee-saved frame and rets into
`fork_child_iret`, which iret's back to ring 3 at exactly the same EIP where
the parent's `int 0x80` returns, with EAX=0 so the child sees `fork() == 0`.

The parent's syscall handler patches `regs->eax = child->pid` and returns
normally — the parent sees `fork() == child_pid`.

`forktest.elf` in `src/userspace/` exercises the full path: a parent
sentinel, a fork, child reads (proves COW visibility), child writes
(triggers four independent COW faults across four 4 KiB-aligned BSS pages),
child exits with status=42, parent's view of every sentinel still original
(proves the parent took its own private copies on its post-yield write
faults).

### 11.2 `execve()` (slice 16a)

Replaces the calling task's address space with a new ELF.  `arch/i386/proc/elf.c`'s
`elf_exec` was extended to free the OLD user PD after the new one is loaded:

```c
task_t *cur = task_current();
uint32_t *old_pd = cur->page_dir;
cur->page_dir = pd;                   /* new PD with the loaded ELF */
tss_set_kernel_stack(...);
vmm_switch(pd);                       /* CR3 swap */
if (old_pd && old_pd != paging_kernel_pd())
    vmm_free_pd(old_pd);              /* reclaim parent-fork inheritance */
ring3_enter(ehdr->e_entry, initial_esp);   /* never returns */
```

The `paging_kernel_pd()` guard preserves the older fresh-task path
(`exec_task_entry`, where "old" PD is the kernel PD shared by all kernel
tasks); only execve from an existing user task actually triggers
`vmm_free_pd`.

The syscall handler (`case SYS_EXECVE` in `arch/i386/proc/syscall.c`) copies
the path string and the argv string array into kernel-side static scratch
*before* calling `elf_exec`, because those buffers live in the about-to-be-
freed user PD.  Statics are safe because syscalls are serialised (cli at
entry) — never two execves in flight.  POSIX requires execve to reset all
caught signal handlers to defaults; this is `sig_task_init(task_current())`.

`execvetest.elf` does fork → child-execve `hello.elf` → parent-survives;
the recipe a real userland shell will use.

### 11.3 `wait4()` + `TASK_ZOMBIE` (slice 16b)

`SYS_WAIT4` (= 114, Linux i386 ABI) reaps a child task and round-trips its
exit status to the parent.  Required adding a fourth lifecycle state:

```
READY ──run──> RUNNING ──exit──> ZOMBIE ──wait4──> DEAD ──reclaim──> READY (new task)
                              │                              ↑
                              └── if parent is kernel-task ──┘  (skip ZOMBIE)
```

Three fields on `task_t`:

- `parent_pid` — set by `task_create` and `task_fork`.
- `exit_status` — written by `SYS_EXIT` (from EBX) before `task_exit`.
- (existing) `state` — gained `TASK_ZOMBIE` between `RUNNING` and `DEAD`.

`task_exit` chooses ZOMBIE vs DEAD based on whether the parent is a live
ring-3 task (`parent->page_dir != paging_kernel_pd()`).  Kernel-internal
tasks (whose parent is the kernel-PD idle/shell) go straight to DEAD —
otherwise the four kernel shell tasks' children would pile up as
unwait4'd zombies and fill the 8-slot pool.  `task_exit` also auto-reaps any
of its own dying zombies (so orphans don't accumulate when their parent
dies without waiting).

`SYS_WAIT4` scans the task pool for the caller's children:

- A matching `TASK_ZOMBIE` is found → copy `exit_status` into the user
  `int *status`, transition the child to `TASK_DEAD` (slot now reclaimable
  by `task_create`), return the child's pid.
- No zombies but live children exist and `!WNOHANG` → `task_yield()` and retry.
- `WNOHANG` and no zombies → return 0.
- No children at all → return `-ECHILD`.

The yield loop runs inside the syscall handler in kernel context; safe
because `task_yield` is re-entrant and the syscall's own kernel stack is
preserved across yields.

`forktest.elf` and `execvetest.elf` were migrated off their original
busy-yield-then-sample pattern to a real `sys_wait4` + status check; serial
now shows:

```
[sys_exit]  task pid=14 status=42 -> task_exit()
[sys_wait4] parent pid=13 reaped child pid=14 status=42
[forktest] REAPED pid=14 status=42
```

### 11.4 Hosted libc and musl state

The fork/exec/wait triad is in, and the branch adds most of the low-level
pieces a static i386 musl binary expects during process startup:

- **ELF auxv.** `elf_exec` writes `AT_PAGESZ`, `AT_RANDOM`, and `AT_NULL`
  after `envp`. Existing Makar crt0 code ignores this, but musl walks it.
- **Anonymous mmap.** `SYS_MMAP2` maps zero-filled anonymous pages into a
  per-task bump window starting at `0x90000000`; `SYS_MUNMAP` unmaps ranges.
  File-backed mmap, `MAP_FIXED`, address reuse, and `mprotect` are still
  absent.
- **TLS.** `SYS_SET_THREAD_AREA` programs GDT slot 6 and loads `%gs = 0x33`.
  The scheduler stores TLS descriptor fields in `task_t` and restores the
  shared GDT TLS slot for TLS-active tasks.
- **FPU state.** The kernel initializes x87/SSE state and saves/restores a
  512-byte FXSAVE image per task, so hosted floating-point code is no longer
  a scheduler-corruption hazard.
- **Startup stubs.** `exit_group`, `set_tid_address`, `rt_sigprocmask`,
  `ioctl`, and `futex` are present as minimal compatibility paths.
- **POSIX-shaped fd surface.** `pipe`, `dup`, `dup2`, `stat`, `fstat`,
  `readdir`, `getpid`, and `getppid` are all exposed.

The remaining musl work is now empirical rather than architectural: compile a
static test binary with the toolchain scaffold, run it, and fill the next
missing syscall or ABI detail it reports. Known likely gaps include `writev`,
full errno conventions, deeper signal-mask semantics, file-backed mmap, and
packaging musl itself into the Makar sysroot.

### 11.5 vfork() and posix_spawn() — no longer needed

The original speculation in this section recommended `posix_spawn` as the
shortcut to running dash without a full COW fork.  That recommendation was
overtaken by reality: native fork-with-COW turned out to be a single weekend
of work (slices 15a-e), and the resulting `task_fork` already covers ~98% of
posix_spawn's use cases when paired with `execve`.  No reason to ship
`vfork`; no need to ship `posix_spawn` either, unless a downstream consumer
asks for it specifically.

### 11.6 An in-kernel C compiler

TCC (Tiny C Compiler, vendored at `vendor/tinycc/`, v0.9.27, LGPL-2.1) is
being ported to run as a **userspace ELF binary** (`tcc.elf`) on Makar.
It compiles C to a static `ET_EXEC` ELF on disk — no fork, no JIT, no
`mmap(PROT_EXEC)`. The workflow is CP/M-style: boot → write source in VIX →
`tcc hello.c -o hello.elf` → `exec hello.elf`.

**Current state (May 2026, v0.9):** all original phases shipped, and
self-hosting now covers the kernel itself.  `tcc.elf` ships on every ISO,
userspace apps (`hello`, `calc`, `sh`, `makbox`) self-rebuild in-OS, and
the bootable Multiboot 2 kernel ELF rebuilds end-to-end with our shipped
TCC via `./build-kernel-tcc.sh` (host-side) or `/apps/rebuild-kernel.sh`
(inside Makar).  Earlier groundwork shipped the kernel-side file I/O
surface (writable fds, `O_CREAT`/`O_TRUNC`/`O_APPEND`, `SYS_STAT`/`FSTAT`,
`SYS_READDIR`, 16 MiB file cap) and the freestanding libc shim
(`malloc`/`stdio`/`setjmp`/`ctype`/`stdlib`/POSIX wrappers), both with
ktest + in-guest-test coverage.  The boot banner reports `gcc-host` /
`tcc-host` / `tcc-in-os` based on the build path.  See
[TinyCC in Makar](tcc.md) for the shipped compiler reference and
[rebuilding the kernel](rebuild-kernel.md) for the maintained guide.

---

## 12. Things worth knowing that don't fit elsewhere

- **Floating point is initialized, but kernel C still avoids it.** The kernel
  uses `fpu_init` and per-task FXSAVE/FXRSTOR so ring-3 FP state survives
  context switches. Kernel code should still avoid C floating-point and keep
  the freestanding integer-only style unless there is a very explicit reason.
- **No SMP**, intentionally. The single biggest implementation simplification
  in the whole codebase. Every "lock" we'd need on SMP is a no-op on UP
  with IF discipline.
- **No virtual memory paging-out**, intentionally. RAM > working set on every
  realistic Makar workload. The infrastructure to swap (page tables,
  reference counting, an LRU walker) is large; the gain is zero.
- **Per-task fd table lives in the heap**, not in the task struct. This means
  a slot-recycled `task_t` doesn't carry stale fds, but it also means
  `task_create` does an allocation. The allocation is amortised by the
  fixed-size task pool — allocations are bounded and visible.
- **The `unkillable` flag on shell tasks** is a hack to keep the four shell
  tasks alive across rogue `kill -9` from a misbehaving userland. Linux's
  equivalent is "PID 1 cannot receive SIGKILL except from itself"; Makar's
  is a single bit checked in `sig_deliver`. Honest about being a hack.

---

## References

- Intel SDM Vol. 3A, ch. 3-5 (segmentation, paging, control registers).
- AMD64 System Programmer's Manual Vol. 2 (the descriptions of CR0/3/4
  flag semantics are clearer than Intel's, even for 32-bit i386).
- *Operating Systems: Three Easy Pieces* (Arpaci-Dusseau), chapters on
  CoW fork, scheduling, paging — pitched at the level your manager will
  recognise.
- OSDev wiki: [Paging](https://wiki.osdev.org/Paging),
  [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) (why we
  *don't*), [TLB](https://wiki.osdev.org/TLB).
- Linux source for sanity-checking the conventions: `arch/x86/include/asm/`
  for IDT, GDT, TSS layout; `kernel/fork.c` for the canonical CoW fork
  implementation Makar's `task_fork` is modelled on; `arch/x86/entry/entry_32.S`
  for the kernel-stack frame shape `fork_child_iret` mirrors.
- musl: `arch/i386/syscall_arch.h` for the syscall shim conventions a future
  port would slot into.
