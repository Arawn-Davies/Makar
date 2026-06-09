---
title: Conventions & style
layout: default
nav_order: 20
---

# Makar — coding style, conventions & practices

The house rules for working in this repo. They **override** generic defaults.
This is the human-facing companion to the `makar-conventions` Claude skill
(`.claude/skills/makar-conventions/SKILL.md`); keep the two in sync — the skill
is the terse checklist, this is the explanation.

Makar is a hobby **i386 (i686) bare-metal OS**: C + AT&T assembly, booted via
GRUB Multiboot 2 (live ISO) and Limine (installed disk), 32-bit protected mode,
higher-half kernel, single CPU, no SMP / PAE / long mode. The whole
build/test/run toolchain is wrapped by `./run.sh` over Docker — no host
cross-compiler required.

---

## 1. Golden rule — *What Would Linux Do?*

When a design or hardware detail is ambiguous, default to the **Linux**
convention: the syscall ABI (`int 0x80`, i386 numbers), driver shape, ACPI / HID
/ AHCI layout, single-user / rescue mode, ANSI/VT100 terminals, `USER_HZ`, the
block layer, `/proc` + `/dev` + `/usr` paths, dotfile names. Prefer documented
references — the Linux source, the OSDev wiki, public-domain libraries
(stb_image, miniz/zlib, doomgeneric) — over hand-rolling. When you follow a
Linux convention in a non-obvious spot, say so in a comment; **do not** write
"WWLD" in code or docs.

---

## 2. Repo layout (where things live)

```
src/kernel/arch/i386/{boot,core,mm,drivers,fs,display,proc,shell,debug}/
src/kernel/kernel/kernel.c        kernel_main + boot sequence
src/kernel/include/kernel/        kernel-internal public headers
src/kernel/include/makar_*.h      the shared kernel⇄userspace ABI (see §6)
src/libc/                         freestanding libc → libk.a (kernel-side)
src/userspace/                    ring-3 ELF apps + the hosted libc.a + gui_* libs
toolchain/                        host musl cross toolchain (separate; its own README)
tests/                            GDB boot-test suite (groups/*.py)
docs/                             this tree; docs/kernel/*.md per subsystem
run.sh iso.sh build.sh generate-hdd.sh getfreedoom.sh copy.sh   build/run scripts
```

`CLAUDE.md` is the **index/router** — start there, follow its pointers to
`docs/`, the companions (`CLAUDE.history.md`, `CLAUDE.roadmap.md`, `SURVEY.md`),
and the source. Never paste detail into `CLAUDE.md`.

---

## 3. Build & run — always through `./run.sh`

Everything goes through `./run.sh` (it wraps the Docker toolchain; falls through
to a host `i686-elf-gcc` or a CI `/.dockerenv` when present).

```sh
./run.sh iso boot          # debug ISO → interactive QEMU
./run.sh iso test          # full regression gate (see §4)
./run.sh iso build         # kernel + makar.iso (+ makar-test.iso when TEST_ISO=1)
./run.sh iso release       # optimised build
./run.sh hdd boot|test|build|release
./run.sh ktest             # fast, headless, kernel-only ktest
./run.sh kbtest [gui]      # in-guest key-injection test
./run.sh gdb iso|hdd       # GDB boot-checkpoint test
./run.sh clean
```

- **One kernel, two ISOs.** `makar.kernel` is built once; `iso.sh` packages
  `makar.iso` (interactive) and `makar-test.iso` (`test_mode` cmdline). There is
  **no compile-time test flag** — the difference is purely `grub.cfg`.
- Debug is `-O0 -g3`, release `-O2 -g`; override via `CFLAGS`. `-j$(nproc)` +
  ccache (`CCACHE=0` to disable).
- Env knobs: `MACHINE=<PMMX|P2|P3|MODERN|…>` picks a QEMU `-cpu`; `RES=<720p|
  1080p|900p|480p|WxH>` bakes `vmode=` onto the booted entry; `MAKAR_USE_KVM=0|1`
  forces the accelerator. KVM is **off** for the correctness gates.
- IDE/clang "missing kernel header" squiggles are expected — the real build uses
  the right include paths.
- Hand a built artifact to the user with `./copy.sh` (stages `makar.iso` to a
  Windows-reachable path); it is gitignored.

---

## 4. Testing — in-guest and headless only

> **No host-driven keyboard, no monitor `sendkey`, no framebuffer scraping. Ever.**

Every test asserts on **COM1 serial markers**. Two mechanisms:

1. **In-guest `.sh` script drivers** — `src/userspace/{shell-smoke,incore,
   libc-tcc}.sh`, run by `./run.sh iso test` after `ktest_run_all()`. Each gates
   per-test PASS/FAIL on `$?`; markers like `SHELL-SMOKE: ALL PASS`. Drive the
   ring-3 shell with `sh.elf -c '<payload>'`; `alloctest.c` is the canonical ELF
   shape.
2. **Key injection** — `./run.sh kbtest`. `keyboard_inject_text/_key()` feed the
   live decode→ring→shell pipeline; `keyboard_test_driver()` (cmdline `kbtest`)
   emits `KBTEST:` markers, for paths where the keystroke itself is under test.

Add coverage in those `.sh` drivers, in `keyboard_test_driver()`, or as a
`ktest` in `proc/ktest.c` (macros in `kernel/ktest.h`) — **never** a host
harness. Full detail: `docs/testing.md`.

**The gates** (run the relevant ones before every commit; state the real
numbers, never assume): `./run.sh ktest`, `iso test`, `guitest`, `kbtest`,
`gdb iso|hdd`. KVM is forced **off** for `ktest`/`gdb`/`nettest`/CI (correctness
— KVM broke GDB breakpoints and masked a TCG-only fault). The `gdb-hdd` content
check is known-flaky (inferior-call timing) — re-run to clear.

---

## 5. Code style

- **Match the surrounding code** — comment density, naming, idiom, brace style.
  Read the neighbours before adding. Kernel C + AT&T asm; **tabs** for
  indentation in kernel C.
- **Comments explain *why***, not *what*. Reference the concrete bug/quirk a
  workaround addresses (e.g. "the 1080p page-fault-at-0xFD400000 bug",
  "VMware reports the CPU 'disabled' on `cli;hlt`"). Function banners describe
  intent + lifecycle.
- **Naming.** File-scope globals are `g_`-prefixed (`g_boot_gui`, `g_sched_
  quantum`). Kernel public symbols are `snake_case`, grouped by subsystem prefix
  (`vesa_`, `vfs_`, `task_`, `pmm_`). Match the module you're in.
- **Higher-half kernel** linked at `0xC0000000` (loaded at phys 1 MiB via
  `AT()`); a low identity window keeps `0x0–0x0FFFFFFF` mapped for phys access.
  Hand **any** kernel pointer to DMA/hardware through `kvirt_to_phys()`
  (`kernel/paging.h`). User space lives below `0xC0000000`.
- **Respect layering — follow the seam, don't punch through it.** The timer IRQ
  never reaches into the display layer; modules register tick hooks
  (`timer_register_tick_hook`). Display presents through the video vtable
  (`kernel/video.h`), not by poking a driver directly.
- **Fallback-gate every hypervisor / hardware / boot-mode-specific change**
  (`vm_kind()`, PCI-id, capability bits, cmdline flags) so the tested QEMU path
  (BIOS + IDE + PS/2 + `-vga std`) is **never** regressed. New accelerated paths
  degrade cleanly to the dumb LFB / VGA-text path.
- **Bound every hardware wait.** Poll loops on device status (8042, IDE DRQ,
  UART, the SVGA FIFO) spin with an iteration cap, never forever — an unbounded
  spin shows up as a "CPU disabled" hang, not a panic.
- **Fail safe, and say what failed.** Every CPU exception is installed (IDT
  0–31); ring-3 faults raise `SIGSEGV`, ring-0 faults panic. Panics render in
  VGA text mode and name the failing context (program/driver + EIP for
  `addr2line`). Prefer a panic with a message over a silent halt.
- **Paths follow Linux:** `/usr` `/apps` `/root` `/proc` `/dev` `/tmp`
  `/mnt/<name>` `/mnt/cdrom`; apps at `/apps/*.elf`, libc at `/usr/lib/libc.a`.
  Per-user dotfiles: `~/.makshrc` (sh login), `~/.vixrc` (vix), `~/.sbrc`
  (text statusbar layout).

---

## 6. The syscall ABI is one source of truth

The ABI is **shared verbatim** between kernel and userspace (Linux-uapi style),
in `src/kernel/include/`:

- `makar_syscalls.h` — the `SYS_*` numbers.
- `makar_abi.h` — clockids, `struct timeval/timespec/stat/dirent`, `tty_cell_t`,
  open/fcntl flags, `S_IF*` + `DT_*` bits.
- `makar_keys.h` — keycodes/sentinels. `makar_signals.h` — signal numbers.

Both `kernel/syscall.h` (kernel-internal dispatch) and `src/userspace/syscall.h`
(the ring-3 wrappers) **include** those headers — they never re-declare the
numbers. Never duplicate or let the two sides drift. Allocate a new syscall
**past the current max**, wire the wrapper, and document it in
`docs/syscalls.md` in the same change. `int 0x80`, i386 register convention.

---

## 7. Commits

- **One commit per discrete work item.** Imperative subject; the body explains
  *why*.
- **NEVER** add `Co-Authored-By`, "Generated with Claude Code", or **any**
  AI-attribution footer — of any kind. (This overrides the global default.)
- Commit/push **only when the user asks.** Always branch off `main`; never commit
  to `main` directly.
- End the body with the gate results you **actually ran** (e.g.
  `ktest PASS; guitest GUI: READY; kbtest ALL PASS`).

---

## 8. Docs — cover everything, update as you go

- Update `docs/` **and `progress.md`** **in the same commit** as the code they
  describe — not at the end of a PR. Any behaviour change or new feature touches
  the docs; mark the matching `progress.md` T-item too.
- **Every** app, library, framework, kernel subsystem, driver, and syscall gets
  documentation: each `mx*` client, the `gui_gfx`/`gui_ui`/`makx`/`gui_browser`
  libs, every `docs/kernel/*.md` module, the syscall table, the glossary. Add an
  element → add its doc.
- `CLAUDE.md` stays a one-line-per-item **router**. Detail goes in `docs/`, the
  companions, or the source.
- New abbreviation → `docs/glossary.md`. New third-party code/asset → keep its
  upstream licence in-tree and record it in `THIRD-PARTY-NOTICES.md` /
  `LICENSES/`. Shipped PRs → `CLAUDE.history.md`; future work →
  `CLAUDE.roadmap.md`.

---

## 9. When unsure

Check `CLAUDE.md` → the relevant `docs/` page → the source, in that order. Ask
the user only for decisions you genuinely cannot resolve from those (and that
change what you build); otherwise pick the obvious option and say so.
