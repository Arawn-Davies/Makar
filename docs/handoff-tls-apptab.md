# Handoff (for Codex): TLS work broke kbtest `app-tab`

## TL;DR
While landing the **TLS gate** (musl `set_thread_area` support), `./run.sh kbtest`
started failing the **`app-tab`** assertion. `alt-tab` and `exit-restore` still
PASS, and `./run.sh iso test` + `ktest` are fully green. The regression is in
the TLS work, **not** a stale build. Find which TLS change breaks the
maktop-spawn-into-named-tab path and fix it (or confirm it's a timing-window
issue in the test and widen it).

## Repro
```sh
./run.sh kbtest          # -> "KBTEST: app-tab FAIL" ; "==> kbtest FAIL"
grep -aE "KBTEST:" kbtest.log
```
Consistent (failed 3×, not flaky). `alt-tab PASS`, `app-tab FAIL`,
`exit-restore PASS`.

## What `app-tab` tests
`keyboard_test_driver()` in `src/kernel/arch/i386/drivers/keyboard.c`:
injects `maktop\n` into a makmux VT shell, `ksleep(300)`, then asserts
`vtty_find_name("maktop") >= 0`. FAIL = maktop didn't register a named tab in
the 300 ms window.

Path: `sh.elf` runs `maktop` → `SYS_VT_OPEN_APP(242)` queues it → **makmux**
polls `SYS_VT_TAKE_APP(248)`, drains, `fork`+`exec`s maktop → maktop calls
`SYS_VT_SETNAME(244)` → `vtty_find_name` then sees it.

## What changed this session (the TLS gate)
**Committed** (HEAD `65f05f7` "feat(tls): per-task %gs plumbing"):
- GDT grown 6→7 entries; TLS slot at index 6 (selector `0x33`), `gdt_set_tls()`
  in `core/descr_tbl.c`.
- `core/isr_asm.S`: the isr **and** irq common stubs **no longer touch `%gs`**
  (removed the kernel-`%gs` load on entry and the `%gs` restore on exit, both
  stubs) so a ring-3 task's TLS `%gs` survives interrupts.
- `task_t` gains `tls_gs/tls_base/tls_limit/tls_pages/tls_active`
  (`include/kernel/task.h`); inited to `tls_gs=0x23, tls_active=0` in
  task_create/idle/fork (`proc/task.c`) and reset on exec (`proc/elf.c`).
- **`proc/task.c` `schedule()`**, right after `task_switch(...)`: reprograms the
  GDT TLS slot (if `tls_active`) and reloads `%gs` from `current_task->tls_gs`.
  **<-- prime suspect.**

**In-tree, UNCOMMITTED** (the rest of the TLS gate, currently in the working
tree — Codex should keep these; they build clean):
- `proc/syscall.c`: `SYS_SET_THREAD_AREA(243)` + `SYS_FUTEX(240)` handlers,
  `#include <kernel/descr_tbl.h>`.
- `include/kernel/syscall.h` + `src/userspace/syscall.h`: Makar extensions moved
  off the Linux numbers musl needs — `SYS_WHOAMI 240→247`,
  `SYS_VT_TAKE_APP 243→248`; added `SYS_FUTEX 240`, `SYS_SET_THREAD_AREA 243`.
- `src/userspace/Makefile`: added `-MMD` + `-include $(wildcard *.d)` header-dep
  tracking (root-cause fix for a *different* bug — see "Ruled out").

## Ruled out
- **Stale userspace build / the renumber.** The userspace Makefile had no header
  dependency tracking, so the first renumber left `makmux.elf` calling the old
  number. Fixed (`-MMD`) and did a clean rebuild (`rm src/userspace/*.o`); both
  headers are now consistent (`vt_take_app=248` kernel+user). **app-tab still
  fails**, so this is not the cause.
- General scheduling/fork/exec: `iso test` (incore = fork/exec/exit heavy) and
  `ktest` pass, and `alt-tab`/`exit-restore` (VT switching) pass.

## Suspects, in priority order
1. **The `%gs` reload in `schedule()`** (`proc/task.c`, just after
   `task_switch`). It runs on *every* context switch. Two failure modes:
   - **Correctness**: maktop is a freshly-`exec`'d task; verify the
     re-entered-vs-fresh paths set `%gs` correctly for a task that's spawned and
     immediately drawn. A wrong `%gs`/GDT reprogram on the switch into a fresh
     fullscreen app could wedge its early syscalls.
   - **Latency**: the per-switch `movw %gs` (+ `gdt_set_tls` for TLS tasks) adds
     overhead; the maktop spawn may now miss the test's 300 ms window.
2. **The `isr_asm.S` `%gs` change** — kernel now runs with the user's `%gs`
   loaded. Confirm nothing in the maktop/makmux path does a `%gs`-relative
   access at ring 0 (kernel is `-fno-stack-protector`, so unlikely, but verify).
3. **The GDT growth to 7 entries** — least likely (iso test passes).

## Suggested bisect (cheap, ~1 build + 1 kbtest each)
1. **Isolate chunk 1**: `git stash` the in-tree changes, `./run.sh kbtest`. If
   app-tab now PASSES, the bug is in the in-tree `set_thread_area`/renumber, not
   the committed `%gs` plumbing. If it still FAILS, the committed `%gs` plumbing
   (`65f05f7`) is the culprit — focus on `schedule()`'s `%gs` reload and the
   `isr_asm.S` change.
2. **Timing vs correctness**: bump the `app-tab` `ksleep(300)` to `ksleep(800)`
   in `keyboard_test_driver()` and rerun. If it PASSES, it's latency, not a
   functional break — either optimize the switch path or just widen the window.
3. **Disable the schedule `%gs` reload** (comment out the `gdt_set_tls(...)` +
   `movw %gs` block after `task_switch` in `schedule()`), rebuild, kbtest. If
   app-tab PASSES, that block is the issue.

## Tree / build notes
- Branch `libc`; HEAD `65f05f7`. The in-tree uncommitted changes are the rest of
  the TLS gate — keep them.
- `./run.sh kbtest` = the only failing gate. `./run.sh iso test` and `ktest`
  pass.
- The end goal (after app-tab is green): a static-musl `int main(){return 42;}`
  linked at `0x40000000` (`./toolchain/cc.sh -static -march=i686
  -Wl,-Ttext-segment=0x40000000`) should `exec` on Makar and exit 42, proving
  TLS+auxv+stubs. The ELF loader (`proc/elf.c`) rejects segments below
  `0x10000000`, so musl must be linked high. See `toolchain/README.md` +
  `toolchain/self-hosting.md`.
