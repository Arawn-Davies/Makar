---
name: makar-conventions
description: Makar OS house rules and style. Use whenever writing, editing, reviewing, committing, testing, or documenting code in the Makar repo (kernel C/AT&T-asm, ring-3 apps, run.sh, docs) so changes match the project's conventions. Apply before opening a PR.
---

# Makar OS — conventions & style

Makar is a hobby i386 bare-metal OS (GRUB Multiboot2, 32-bit protected mode,
QEMU, Docker build via `./run.sh`). Follow these rules exactly; they override
generic defaults. `CLAUDE.md` is the index — read it and the `docs/` it points
to before non-trivial work. The fuller human-facing version of this checklist is
**`docs/conventions.md`** — keep the two in sync.

## Golden rule — default to the Linux convention
When a design or hardware detail is ambiguous, default to the **Linux**
convention (syscall ABI, driver shape, ACPI/HID/AHCI layout, single-user mode,
ANSI/VT100, USER_HZ, block layer). Lean on documented references (Linux source,
OSDev wiki, public-domain libs like stb_image/miniz) over hand-rolling.

## Commits
- **One commit per discrete work item.** Imperative subject, body explains *why*.
- **NEVER** add `Co-Authored-By`, "Generated with Claude Code", or any
  AI-attribution footer of any kind.
- Commit/push only when the user asks. Branch off `main`; never commit to `main`.
- End each commit body with the gate results you actually ran (e.g.
  "ktest 875/0; guitest GUI: READY").

## Testing — in-guest and headless only
- **No host-driven keyboard, no monitor `sendkey`, no framebuffer scraping. Ever.**
- Two mechanisms, both asserting on COM1 serial markers:
  1. In-guest `.sh` script drivers (`src/userspace/{shell-smoke,incore,libc-tcc}.sh`)
     run by `./run.sh iso test` after `ktest_run_all()`.
  2. Key injection (`./run.sh kbtest`) via `keyboard_inject_text/_key()` +
     `keyboard_test_driver()` emitting `KBTEST:` markers.
- Add coverage there or as a `ktest` in `proc/ktest.c` (`kernel/ktest.h` macros) —
  never a host harness.
- Gates: `./run.sh ktest`, `iso test`, `guitest`, `kbtest`, `gdb iso|hdd`.
  KVM is OFF for ktest/gdb/CI (correctness); the gdb-hdd content check is known
  flaky — re-run to clear.
- Run the relevant gates before every commit; state real results, never assume.

## Build / run
- Everything goes through `./run.sh` (wraps the Docker toolchain). Single kernel,
  two ISOs (`makar.iso` interactive, `makar-test.iso` test_mode) differing only
  in `grub.cfg`. `iso build` emits only `makar.iso` unless `TEST_ISO=1`.
- IDE/clang "missing kernel header" errors are expected; the build uses the right
  include paths.

## Code style
- **Match the surrounding code** — comment density, naming, idiom. Kernel C +
  AT&T asm. Read the neighbours before adding.
- Higher-half kernel at `0xC0000000`; hand any kernel pointer to DMA/hardware via
  `kvirt_to_phys()`. User space below `0xC0000000`.
- **Syscall ABI is one source of truth**: numbers/signals/keys/typed structs in
  `src/kernel/include/makar_{syscalls,signals,keys,abi}.h`, included by kernel
  *and* userspace. Never duplicate or drift. Allocate new numbers past the
  current max; document in `docs/syscalls.md`.
- Respect layering: e.g. the timer IRQ never reaches into the display layer —
  modules register tick hooks. Follow the existing seam, don't punch through it.
- **Fallback-gate** every hypervisor/hardware-specific change (`vm_kind()`,
  PCI-id, boot-mode, capability bits) so the tested QEMU (BIOS+IDE+PS/2) path is
  never regressed.
- Paths follow Linux: `/usr` `/apps` `/root` `/proc` `/dev` `/mnt/<name>`;
  apps at `/apps/*.elf`, libc at `/usr/lib/libc.a`.

## Docs — cover everything, update as you go
- Update `docs/` **in the same commit** as the code it describes — not at the end.
- Every app, component, framework, and library gets documentation: each `mx*`
  client, the `gui_gfx`/`gui_ui`/`makx`/`gui_browser` libs, kernel subsystems and
  drivers, syscalls, the glossary. If you add an element, add its doc.
- `CLAUDE.md` is an **index/router only** — do not paste detail into it. Put detail
  in `docs/`, the companion files (`CLAUDE.history.md`, `CLAUDE.roadmap.md`,
  `SURVEY.md`), or the source, and add/keep a one-line pointer in `CLAUDE.md`.
- New abbreviations → `docs/glossary.md`. New third-party code/assets → keep the
  upstream licence in-tree and record it in `THIRD-PARTY-NOTICES.md` / `LICENSES/`.
- Record shipped PRs in `CLAUDE.history.md`; queue future work in
  `CLAUDE.roadmap.md`.

## When unsure
Check `CLAUDE.md` → the relevant `docs/` page → the source. Ask the user only for
decisions you genuinely can't resolve from those; otherwise pick the obvious
option and say so.
