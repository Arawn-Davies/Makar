---
title: Roadmap
parent: Development
nav_order: 1
---

# Roadmap

Makar's roadmap is no longer about proving that ring-3 programs, fork/exec,
or in-OS compilation are possible. Those foundations exist. The next phase is
about turning the system into a more coherent hosted environment while keeping
the kernel and tests small enough to reason about.

For shipped milestones, see [History](history.md). This page is about current
direction.

## Current Baseline

The current tree has:

- Multiboot 2 i386 kernel boot
- VESA framebuffer terminal and virtual terminals
- PS/2 keyboard driver with in-guest injection tests
- FAT32, ext2, ISO9660, tmpfs, devfs, procfs, logfs
- ring-3 ELF loading
- copy-on-write `fork`
- `execve`
- `wait4`
- signals with user handlers
- fd tables, pipes, `dup`, `dup2`
- userspace shell with pipes/redirection/list operators/background jobs
- TinyCC running inside the OS
- kernel rebuild workflow using TCC
- userspace libc shim
- x87/SSE FPU state per task
- anonymous mmap
- ELF auxv
- i386 TLS through `set_thread_area`
- static-musl toolchain scaffold
- deterministic in-guest tests

## Near-Term Priorities

### 1. Static musl smoke binary

Goal: build a tiny static i386 musl ELF on the host toolchain and run it under
Makar.

Initial target:

```c
int main(void) { return 42; }
```

Known substrate already present:

- high user ELF load region
- auxv
- anonymous mmap
- TLS
- startup syscall stubs
- FPU state

Likely remaining work:

- verify exact musl entry/startup syscall path
- implement `writev` if the selected musl build uses it
- normalize errno conventions needed by libc wrappers
- package musl headers/libs into an optional sysroot once a binary runs

Success criterion:

- a static musl test app exits with the expected status under Makar
- the repro and link flags are documented

### 2. Strengthen POSIX wrapper consistency

The syscall surface is broad enough to run meaningful programs, but return and
errno behavior is still mixed between old Makar-style `-1` paths and newer
errno-shaped wrappers.

Work items:

- audit userspace wrappers for errno behavior
- document each syscall's error convention
- add tests for representative failure paths
- avoid changing kernel ABI casually; fix wrappers first where possible

### 3. Improve terminal compatibility

Makar currently exposes terminal behavior through Makar-specific syscalls.
That is fine for native apps but limits portable programs.

Possible steps:

- minimal `isatty` behavior beyond `ioctl -> -ENOTTY` if needed
- termios-shaped stubs for programs that only probe and continue
- better `/dev/tty*` semantics
- clearer foreground/background ownership for shell jobs

Full POSIX job control remains a long-term project.

### 4. Filesystem robustness

The installed-system story depends on writable storage being predictable.

Work items:

- keep FAT32/ext2 behavior aligned through VFS
- harden flush/unmount paths
- improve installer workflows
- document rootfs election and installed layouts
- add more tests for write, rename, mkdir, and persistence paths

### 5. Keep test determinism

The in-guest testing model is now a core project asset.

Rules:

- no reintroduction of host keyboard typing automation
- add shell/app coverage through in-guest scripts
- add keyboard/focus coverage through `kbtest`
- preserve serial markers for machine-readable test results
- keep `./run.sh ktest`, `./run.sh kbtest`, and `./run.sh gui all-tests` green

## Medium-Term Themes

### Hosted libc and userland

Static musl is the next compatibility milestone. After a minimal binary runs,
the path becomes demand-driven: run real small programs, fill missing syscalls,
and keep documenting what was required.

Likely targets:

- small libc test programs
- simple POSIX utilities
- a more standard shell package if the syscall surface becomes sufficient

### Shell completion

`/apps/sh.elf` is usable but not POSIX-complete.

Useful follow-ups:

- command substitution
- shell functions
- `case`
- `trap`
- `export` with real envp propagation
- more precise quote/expansion ordering
- richer script smoke tests

### In-OS development experience

TinyCC works. The next work is polish:

- clearer editor/compile/run loop
- better examples
- persistent project directories on installed systems
- better compiler diagnostics on screen
- maybe `make`-like small build scripts later

### Networking

Networking is still open. A likely path is:

1. choose a simple emulated NIC target
2. bring up packet RX/TX
3. integrate a small TCP/IP stack
4. add DHCP/DNS
5. build tiny clients/servers

This should come after the hosted userspace and filesystem paths are less
volatile.

### Makar and Medli Interop

Makar and Medli remain sibling projects. Good sideport candidates are:

- filesystem layout conventions
- small app behavior
- shell command vocabulary
- documentation patterns
- test marker conventions

Architecture-specific details, such as Makar's i386 GDT/TLS work, should not
be sideported directly.

## Long-Term Ideas

These are not near-term commitments:

- x86-64 port
- SMP
- demand paging or swapping
- dynamic linking
- full users/permissions
- POSIX sessions and job control
- GUI beyond framebuffer terminal/apps
- GCC or binutils running inside Makar

Each of these is large enough to require a clear user-facing reason before
implementation.

## Release Quality Bar

For any major branch:

```sh
./run.sh ktest
./run.sh kbtest
./run.sh gui all-tests
```

should pass locally, and docs should be updated in the same PR when behavior
changes.
