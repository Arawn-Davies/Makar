# Plan: POSIX shell + TCC self-host roadmap

## Context

The userspace-shell migration (PR #180) shipped: `/apps/sh.elf` is the
default login shell on every VT, admin syscalls go through
`task_is_admin()`, libc + TCC tests run via in-OS `sh_run_file`
scripts instead of HMP sendkey.

The shell now has parity with the in-kernel one, but it's still a
*long* way from POSIX 2017, and TCC self-hosting is not quite proven
end-to-end.  This plan picks the next slices for both tracks and flags
one blocker to clear before serious scripting work resumes.

## Stale-plan cleanup

After this plan lands, mark these companion files in `docs/plans/`:

- `userspace-shell-migration.md` — **shipped** (PR #180 merged).
- `userspace-shell-migration-HANDOFF.md` — keep; source of truth for
  known issues (notably the `sh_script.c` nested-script bug).  Point
  its "follow-ups" section at this roadmap.
- `tcc-rebuild-coverage.md` — **superseded**.  Planned to add HMP
  `test_tcc_rebuild_*` scenarios, but those moved into
  `/src/userspace/libc-tcc.sh` (PR #180).
- `posix-shell.md` — keep; it's the comprehensive feature inventory.
  This roadmap *references* it instead of duplicating.

## Blocker to clear first

### `sh_script.c` nested-`sh <file>` parser-state leak

Symptom (caught running `shell-smoke.sh`): after a nested
`sh /apps/foo.sh` call returns, every subsequent `if/then/else/fi`
block — multi-line *or* inline — fires **both** the `then` *and* the
`else` branch.  The trailer's own `if [ $fail -eq 0 ]` block also
double-fires.

Source: `src/kernel/arch/i386/shell/sh_script.c` (808 lines).  The
if-branch terminator scan at line 472 mentions "Find the matching
`fi`, ignoring nested if/fi" — likely related state isn't reset when
`sh_run_file` recurses.

Why it blocks both tracks:

- POSIX shell work expands sh.elf scripting; any script that sources
  another (`sh /apps/lib.sh; …`) hits the same shape.
- TCC self-host wants `libc-tcc.sh` to grow more nested helpers
  (e.g. one rebuild-and-run helper that other entries call); blocked.

**Slice**: focused fix in `sh_script.c`, with a regression test in
`shell-smoke.sh` that nests a small `sh /tmp/probe.sh` and asserts
exactly one branch fires.  **This is the first slice on
`feat/tcc-progress`.**

## Track A: POSIX shell — first three slices

Full ladder lives in [`posix-shell.md`](posix-shell.md).  Concrete
next slices:

### A1: pipes (`cmd1 | cmd2`)

- **Kernel**: `SYS_PIPE` (returns two fds via out-buffer), `SYS_DUP2`,
  new `FD_KIND_PIPE` with a 4 KiB ring per pipe (blocking read on
  empty, blocking write on full).  `fd.h` already comments the slot
  layout was designed for pipe(2)/dup(2) — no fd-table shape change.
- **`sh.elf`**: tokenize on `|`; per-stage fork; in each child after
  fork, `dup2(pipefd[0/1], STDIN/STDOUT_FILENO)` then `execve`.
- **Test**: extend `shell-smoke.sh` with `echo hello | cat`, assert
  serial contains `hello` on a single line.

### A2: redirection (`<`, `>`, `>>`, `2>`)

- **Kernel**: nothing new (uses existing `SYS_OPEN` + the new
  `SYS_DUP2` from A1).
- **`sh.elf`**: tokenizer recognises the operators + filename; in the
  forked child, `open(file, …)` + `dup2(fd, target)` before `execve`.
- **Test**: `echo hello > /tmp/redir.txt; cat /tmp/redir.txt`.

### A3: `&&` / `||` / `&` + minimal job control

- **Kernel**: nothing new.  Background tasks need `SIGCHLD` reaping,
  but a tiny `jobs[]` table in sh.elf + manual `wait` builtin is
  enough for v1.
- **`sh.elf`**: parser splits on `&&`, `||`, `&`; tracks the last
  exit status to gate `&&`/`||`; for `&` adds the pid to `jobs[]`
  and doesn't `wait4` synchronously.
- **Test**: `true && echo yes`, `false || echo no`, `sleep 1 & wait`.

Slices A4–A7 (command substitution, quoting, functions, full job
control) stay deferred to `posix-shell.md`.

## Track B: TCC self-host gap-fill

### B1: libc stdio gaps (`fseek` / `ftell` / `remove` / `execvp`)

- **What**: `src/userspace/stdio.c` has only `fread`/`fwrite`;
  `stdlib.c` has no `remove` or `execvp`.  `tcctools.c` warns on all
  four during the TCC build.
- **Implementation**: tiny libc additions:
  - `fseek` = `sys_lseek(fileno(f), offset, whence)` + clear EOF flag.
  - `ftell` = `sys_lseek(fileno(f), 0, SEEK_CUR)`.
  - `remove` = `sys_unlink`.
  - `execvp` = PATH walk + `sys_execve`; kernel-shell already does
    the equivalent in `shell.c:run_external` — lift that into libc.
- **Test**: extend `libc-tcc.sh` with a compile-only TCC build that
  uses each function (or just verify `tcc /src/userspace/tcc.c …`
  emits no implicit-decl warnings).
- **Critical files**: `src/userspace/stdio.c`, `src/userspace/stdio.h`,
  `src/userspace/stdlib.c`, `src/userspace/stdlib.h`.

### B2: `tcc` rebuilds `tcc.elf` in-OS

- Add to `libc-tcc.sh`:
  `tcc /src/userspace/tcc.c -o /tmp/tcc-rebuilt.elf` (and the linked
  `libtcc1.a` build).  Marker `LIBC-TCC: [PASS] tcc-self`.
- This is the self-hosting flex.  Concrete milestone for the version
  bump.

### B3 (stretch): `mmap(PROT_EXEC)` + `tcc -run foo.c`

- **Kernel**: new `SYS_MMAP` (anonymous, executable user pages) +
  `SYS_MUNMAP` for cleanup.
- **TCC**: enables `-run` mode — `tcc -run foo.c` compiles + executes
  in-process without a separate `exec` step.
- **Test**: `tcc -run /src/userspace/hello.c` prints expected
  greeting; user pages reclaimed after exit.

## Cross-track interaction

The two tracks unlock each other:

- After **A1 + A2**: `tcc foo.c -o foo.elf 2> /tmp/errors.log &&
  exec foo.elf` works.
- After **A4** (command substitution): `for f in $(ls *.c); do tcc $f;
  done` works.
- After A1 lands, `shell-smoke.sh` can do content-level assertions via
  `cat /proc/meminfo | grep MemFree` — currently it can only check
  `$?`.

## `MAKAR_VERSION 1.0` definition

1.0 ships when the *full* OS is self-hosting from inside Makar:

1. **TCC rebuilds `tcc.elf`** in-OS (Track B Slice B2 milestone).
2. **Kernel rebuilds itself** — `tcc` + the kernel build's preprocess
   chain + a linker driver, all in-OS.  Bigger lift; may require
   porting an in-OS `ld` or accepting `makar.ld` as a hand-curated
   linker script TCC can drive.
3. **Bootloader story owned**: install Limine (or successor) from
   inside the running OS onto an attached HDD via the existing
   block-device fd path — replaces the host-side `generate-hdd.sh`.

Until all three demonstrably work from a running Makar instance, the
version stays 0.x.

## Critical files

Existing surfaces to lean on rather than rebuild:

- `src/kernel/include/kernel/fd.h` — `FD_KIND_*` enum + `fd_alloc` /
  `fd_get` API; explicitly designed for pipe(2)/dup(2).
- `src/kernel/arch/i386/proc/syscall.c` — admin syscall section is the
  template for new syscalls.
- `src/kernel/arch/i386/proc/task.c` — `task_fork`, `task_wait4`
  (for job-control reaping).
- `src/userspace/sh.c:run_external` — PATH walk to lift into libc
  `execvp`.
- `src/userspace/syscall.h` — pattern for stub wrappers.
- `src/userspace/libc-tcc.sh` + `src/userspace/shell-smoke.sh` —
  marker conventions to mirror in new tests.

## Verification per slice

- **sh_script.c fix**: nested-`sh` regression test in
  `shell-smoke.sh`; `./run.sh gui smoke` shows the new test PASS once.
- **A1 pipes**: `echo X | cat` round-trip in `shell-smoke.sh`.
- **A2 redirection**: `echo X > /tmp/r.txt; cat /tmp/r.txt` round-trip.
- **A3 &&/||/&**: per-operator one-liners in `shell-smoke.sh`.
- **B1 libc stdio gaps**: TCC compile of any libc-using source emits
  no implicit-decl warnings.
- **B2 tcc-self**: `LIBC-TCC: [PASS] tcc-self` on serial.
- **B3 mmap/-run**: `tcc -run /src/userspace/hello.c` prints expected
  greeting; no kernel panic; user pages reclaimed after exit.

## Out of scope

- ksh array syntax, process substitution `<(cmd)`, brace expansion —
  see `posix-shell.md` "Out of scope".
- Full job control (`fg`/`bg`/`tcsetpgrp` + process groups) — too big
  for this roadmap; queue once basics are in.
- gcc port — TCC + libc is the self-hosting target.  gcc is a
  separate (much bigger) initiative.
