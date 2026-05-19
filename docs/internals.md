---
title: Internals
layout: default
nav_order: 9
permalink: /internals
---

# Makar internals: a technical walkthrough

This document is for readers who want the *why*. The per-subsystem reference
pages (`docs/kernel/*.md`) explain *what* each module does; this one walks the
machine top-to-bottom — CPU state at boot, paging, TLB management, per-task
address spaces, the scheduler, ring-3 entry, the syscall ABI — and finishes
with a frank assessment of what stands between the current model and a real
POSIX/libc userspace (`fork()`, signals under fork, musl/dash).

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
which puts it above the BIOS legacy area and BDA. No high-half mapping — we
run identity-mapped in low memory. This is a deliberate simplification: a
high-half kernel would force every page directory to mirror the high PDEs and
makes early-boot debugging fiddlier; the cost is that user-space addresses
above 256 MiB can't be used by ring-3 (we cap user at the kernel identity
window, see §5).

---

## 2. Descriptor tables

### GDT

Six entries, all flat (base 0, limit 4 GiB):

| Selector | Index | DPL | Type | Use |
|---|---|---|---|---|
| `0x00`   | 0 | -   | null         | required |
| `0x08`   | 1 | 0   | code, exec/read | kernel CS |
| `0x10`   | 2 | 0   | data, read/write | kernel DS/ES/FS/GS/SS |
| `0x18`   | 3 | 3   | code, exec/read | user CS |
| `0x20`   | 4 | 3   | data, read/write | user DS/ES/FS/GS |
| `0x28`   | 5 | 0   | TSS (32-bit available) | task-state, see below |

Three things matter here:

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

### IDT

256 entries, all 32-bit interrupt gates. Generated stubs in `isr_asm.S` push
a fake error code (where the CPU didn't), push the vector number, and jump to
`isr_common_stub` which pushes the full register set, calls into C, and
restores. The interrupt gate type clears IF on entry; we keep it clear for
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

`debug/page_fault.c` decodes these and prints a panic screen. There is no
page-fault *handler* yet — every fault is fatal. The eventual demand-paged
allocator and CoW (§11) live here.

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
   - `USER_STACK_TOP = 0xBFFF0000` (PDE 767) — one 4 KiB page for stack.

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

`src/kernel/arch/i386/proc/task.c`. Fixed-size pool of 8 `task_t` slots,
round-robin scheduler, voluntary `task_yield()` *and* preemptive timer-driven
yields (PIT 100 Hz, `SCHED_QUANTUM=4` ticks → 40 ms time slice).

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

`task_exit` flips state to `TASK_DEAD` and yields. The next `schedule()`
that runs on a different PD frees the dead task's PD via `pmm_free_frame`
(plus the heap-allocated PT pages it owns). We deliberately don't free the
PD inline because the current CR3 may *be* that PD; the CR3 switch in
`vmm_switch` must happen before we hand the frame back to the PMM. This is
exactly the "delayed reaper" pattern Linux uses for `free_task_struct`.

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
copies the program text in, maps a user stack page below `USER_STACK_TOP`,
constructs argc/argv on that stack, and jumps through `ring3_enter`. We have
no dynamic loader; binaries are statically linked freestanding ELFs.

---

## 9. Syscall ABI

`int 0x80`, Linux i386 convention. EAX = syscall number; EBX/ECX/EDX/ESI/EDI
= arg0..arg4. Return value comes back in EAX.

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
The cost is latency — a slow syscall (FAT32 read, IDE PIO) holds the IRQ
mask for milliseconds. Acceptable on a single-task interactive shell; the
fix is to *enable* IF inside the handler once we're past the regs-save and
no longer running on a possibly-corrupt stack, the same pattern Linux uses
for `local_irq_enable()` inside its syscall path. The plumbing is there
(`irq_save_disable` etc.); we just haven't pulled the trigger.

The full table lives at `src/kernel/include/kernel/syscall.h`. Numbers 1..49
match Linux/i386 exactly (EXIT, READ, WRITE, OPEN, CLOSE, LSEEK, KILL, BRK,
SIGNAL, FCNTL...). 100, 158, 200–217 are Makar-only extensions for terminal
ops, signal returns, and the new pixel framebuffer API.

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

## 11. The road to fork() and a POSIX libc

This is the section where the hand-waving stops. Below is the actual sequence
of things that would have to land, in order, for `dash` (or, eventually,
`bash`) to run.

### 11.1 `fork()`: what's actually involved

POSIX `fork()` semantics:
1. Atomically create a child process that is a copy of the parent, except
   for: a different PID, a different parent PID, copies of all open file
   descriptors that share the underlying open-file descriptions (so seeks
   in the child see the parent's seeks), the child sees `fork() == 0`,
   the parent sees `fork() == child_pid`.
2. The child's address space is a logical copy of the parent's — same
   contents, **independent writes**. Almost universally implemented as
   **copy-on-write**: both PDs share physical frames, both marked
   read-only, the page-fault handler clones a frame on the first write.

On Makar today:

- **PD clone.** Easy. Walk the parent PD, for each present PTE in the user
  range allocate a new frame, copy the contents, install in the child PD.
  CoW is an optimisation, not a correctness requirement.
- **CoW.** Walk both PDs, set `WRITABLE=0` on every shared PTE, set a custom
  software bit in the PTE flags (we have three reserved bits, `[11:9]`, that
  the CPU ignores) marking it CoW. Add a page-fault handler that intercepts
  W/R=1, P=1, our-cow-bit=1 faults: allocate a new frame, copy, install
  writable in the faulting PD, decrement a refcount on the original frame,
  free if zero. The refcount needs a `pmm_ref_inc`/`pmm_ref_dec` API. None
  of this is novel; it's a 200-line patch with no architectural blockers.
- **PID allocation.** Today PIDs are dense `pool_index + 2`. `fork()` needs
  a monotonic counter that doesn't repeat for the lifetime of the system
  (otherwise `waitpid` can race with PID reuse). Trivial: bump-allocate from
  a `uint32_t s_next_pid`.
- **fd-table dup.** The fd table is per-task (`kernel/fd.h`). `fork` must
  duplicate it, sharing the *open-file descriptions* — i.e., the underlying
  `vfs_node_t *` + offset state — rather than the descriptors. Today the
  fd-table entry IS the (kind, vnode, off) tuple, with no separate
  open-file struct. To make `seek` in the parent visible in the child after
  fork (POSIX requirement), the open-file state needs to be heap-allocated
  with a refcount, and the fd entry becomes a pointer to it. ~150 lines.
- **Return-value split.** The parent returns the child PID; the child
  returns 0. Our `task_create` returns `task_t *`; the caller of `fork` is
  the parent (and will see the child PID). The child's first instruction
  is whatever EIP we put in its saved-register frame; we set EAX=0 in the
  frame and the saved-EIP to the instruction *after* the `int 0x80` from
  `fork`. The cleanest model is to enter the child through a small assembly
  trampoline that loads zero into EAX and jumps to the saved user EIP from
  the fork-syscall sigframe. Linux does the same.

Estimated effort: **a weekend, plus a week of test-suite hardening**.

### 11.2 vfork() vs posix_spawn()

If you only need fork-then-immediately-exec (which is what the shell does
~98% of the time), there are two cheaper alternatives:

- **`vfork()`**: child shares the parent's address space and the parent is
  *suspended* until the child execs or exits. Avoids the CoW dance entirely.
  Used to be the standard "fast fork" before CoW. Still in POSIX.1-2001 as
  legacy. We could implement it in an afternoon: child gets a fresh task_t
  with the parent's PD borrowed (no clone), parent's `wait_for_child` flag
  set, scheduler skips the parent until child exits or execs (which switches
  to a new PD).
- **`posix_spawn()`**: a single syscall that does fork+setup+exec in the
  kernel. No address-space duplication needed at all — the kernel allocates
  a fresh PD, loads the new image, installs file actions. This is what musl
  ships and what Android's bionic relies on heavily. **This is the cheapest
  path** to running `dash` and is the route I'd take first.

The downside of starting with vfork/spawn rather than fork is that anything
that calls `fork()` without an `exec()` (e.g., a shell that wants to run a
function body in a subshell, or `python -c 'import os; os.fork()'`) breaks.
For an interactive shell that's a real limitation. So: ship posix_spawn
first to unblock dash; ship fork+CoW second to unblock the long tail.

### 11.3 Signals under fork

We already have a per-task `sig_handlers[NSIG]` table and Linux-style mask /
pending bitmaps. `fork()` must:

- Copy `sig_handlers` byte-for-byte (POSIX: child inherits dispositions).
- Copy `sig_mask` (POSIX: child inherits mask).
- **Clear `sig_pending`** (POSIX: pending signals are NOT inherited).
- Inside the child, on first return-from-syscall, `sig_deliver` runs as
  usual against the cleared pending set.

Pretty mechanical. The non-trivial part is the *sigframe* on the user
stack: if a signal handler is mid-flight at the moment of fork, the child
inherits both the alternate ring-3 stack frame and the pending handler. We
already build a sigframe in `signal.c` for `SYS_SIGNAL` user handlers; the
trampoline (`SYS_SIGRETURN`) restores cleanly. Fork inherits it without
modification.

### 11.4 musl libc port

musl is small, MIT-licensed, designed for embedded/static linking, and has
a very thin syscall shim layer (`musl/arch/i386/syscall_arch.h`). The port:

1. **Static-only.** No dynamic linker yet (we'd need `ld-musl.so.1`,
   `dlopen`, and rtld). Compile with `musl-gcc --static`.
2. **Syscall surface.** musl uses ~110 Linux syscalls. We have ~30. The
   list of gaps that block a static `dash` build (in priority order):
   `dup2`, `pipe`, `wait4`, `waitpid`, `getpid`, `getppid`, `umask`,
   `chdir` (we have `cd` builtin only — fix that),  `fstat`/`stat`,
   `getdents` (we have `SYS_LS_DIR` which returns a pre-rendered blob; a
   `getdents`-style streaming API is needed for `opendir`/`readdir`),
   `ioctl` (at least `TIOCGWINSZ` for terminal size — we have `SYS_TERM_SIZE`
   to plumb to that).
3. **`brk`/`sbrk`.** Done (SYS_BRK 45). musl's allocator uses it directly.
4. **`mmap` (anonymous).** musl's allocator falls back to `mmap` for large
   allocations. We don't have it. Implementing `mmap(MAP_ANONYMOUS)` as a
   call to `vmm_map_page` for an arbitrary range is straightforward; the
   tricky bit is virtual-address allocation (we need a per-task "mmap arena"
   with a free-region tree). A bump allocator from `0xC0000000` downward is
   the laziest correct option.
5. **TLS.** musl wants `set_thread_area` (i386's old-school per-task GDT
   slot for FS). Set up GDT entry 6 as a per-task TLS slot, write its base
   on context switch. ~50 lines.

Estimated effort: **two weekends, plus a week of debugging static-linked
dash booting cold**. The standard arithmetic on a libc port is that the
first 80% takes 20% of the time and the last 20% (vfork-not-fork-special-
cases, restartable syscalls, signal-safe libc primitives) takes the rest.

### 11.5 Why dash before bash

Both are POSIX shells. dash is ~150 KiB, no GNU extensions, no command-line
editing, no job control beyond the POSIX minimum. bash is ~1 MiB, depends on
readline (which depends on termcap, which depends on terminfo lookup —
another two-day port), and uses ~20 more syscalls than dash (most around
job control and signal-safety in interactive mode).

The realistic path: ship posix_spawn → port musl → build static dash → fix
the gaps that dash exposes (there will be three or four) → declare victory.
bash-on-Makar is a separate, larger project.

### 11.6 An in-kernel C compiler

`tcc` (the Tiny C Compiler, ~200 KiB) compiles C to ELF in memory and writes
the output via `vfs_write_file`. No fork needed — it's a single-binary
operation. Once musl is static, building tcc against it gives us a
self-hosting "write source, compile, run" loop on bare metal — the CP/M
target. This is the slice we'd actually demo to your manager once §11.1–11.4
land.

---

## 12. Things worth knowing that don't fit elsewhere

- **No floating point in the kernel.** `-mno-sse -mgeneral-regs-only` in
  CFLAGS. Saves us from having to save/restore FPU state on every context
  switch. User tasks aren't gated from FP (we just haven't set `CR0.MP` or
  installed an `#NM` handler, so the first user FP instruction faults).
- **No SMP**, intentionally. The single biggest implementation simplification
  in the whole codebase. Every "lock" we'd need on SMP is a no-op on UP
  with IF discipline.
- **No virtual memory paging-out**, intentionally. RAM > working set on every
  realistic Makar workload. The infrastructure to swap (page tables,
  reference counting, an LRU walker) is large; the gain is zero.
- **Per-task fd table lives in the heap**, not in the task struct. This means
  a slot-recycled `task_t` doesn't carry stale fds, but it also means
  `task_create` does an allocation. The allocation is amortised by the
  fixed-size 8-task pool — we hit `kmalloc` at most 8 times per boot.
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
  implementation.
- musl: `arch/i386/syscall_arch.h` and `src/process/posix_spawn*` for the
  fast-path-without-fork model we'd port first.
