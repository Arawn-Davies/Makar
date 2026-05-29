# Handoff — Ctrl-C cascade fix, sh.elf inline-if, UI-test isolation

_Branch: `feat/tcc-progress`. Written 2026-05-29. Compacted session context for pickup._

## Why this work happened

`./run.sh iso test` ended on the UI phase reporting `ui_test: 8/20 passed`. Investigation
showed **1 real failure** (`ctrlc-kills-child`) that then **cascaded** into 11 more
scenarios (all `got: (empty)` serial). Root cause was a kernel signal-handling bug; the
cascade was amplified by the harness design (single shared QEMU + a `reset_shell` that
couldn't recover a wedged shell).

## Root cause (fixed)

The UI suite runs a **single shared QEMU**; the VT0 login shell is **`/apps/sh.elf`** run
as the kernel task `mak.sh0` (kernel.c:`user_shell_slot_entry` → `elf_exec /apps/sh.elf --login`).

`exec /apps/calc.elf` in sh.elf does fork+execve+wait4. Ctrl-C delivers `SIGINT` to the
focused child (calc). The default-terminate path in `sig_deliver` (signal.c) set the victim
**straight to `TASK_DEAD`**, but `wait4` (syscall.c:`SYS_WAIT4`) only reaps `TASK_ZOMBIE`
children — and the keyboard-focus hand-back (`keyboard_set_focus(me)`) lives **only** in the
ZOMBIE-reap branch. So a SIGINT-killed foreground child was never reaped, focus stayed on the
dead child, the typed `pwd` was dropped, and the shared shell wedged → every later scenario
got empty serial → cascade.

`task_exit` (normal SYS_EXIT) already had the correct ZOMBIE-vs-DEAD decision (leave a zombie
when the parent is a live ring-3 task). The signal path simply bypassed it.

### Fix (kernel)
- **`task.c`**: extracted `task_terminate(task_t *t, int status)` — sets `exit_status` and
  applies the same zombie-vs-dead decision `task_exit` used. `task_exit` now calls it.
- **`task.h`**: declared `task_terminate`.
- **`signal.c`**: both default-terminate paths (SIGKILL + generic terminate) now call
  `task_terminate(t, 128 + sig)` instead of `t->state = TASK_DEAD`. Status `128+signo` is the
  POSIX shell convention (SIGINT → 130, which `ctrlc-cat` asserts). The `unkillable` check
  still runs first, so protection is unchanged.
- Safe for the in-kernel rescue shell: its `exec` children have a kernel-PD parent →
  `task_terminate` still picks `DEAD`, so `shell_exec_elf`'s `while (state != DEAD)` still ends.

**Result: 20/20 UI scenarios pass (verified twice, consecutively).** Kernel ktests fully green.

## sh.elf inline-if parser fix

Removing the cascade exposed a second pre-existing bug: `incore` failed with `sh: expected
'then'`. sh.elf's script block parser (`run_block_until` in `src/userspace/sh.c`) required
`then`/`else`/`fi`/`do` as standalone `;`/newline segments and rejected the inline form
`if COND; then BODY; else BODY; fi`. demo.sh uses the same inline form (its `test_demo_script`
isn't in the default suite, so it was never caught).

### Fix (`src/userspace/sh.c`)
- `parser_t` gained a one-statement pushback: `char pending[]; int has_pending;`
  (both zero-initialised — see the two `parser_t … = {0}` sites).
- New `rb_next(p, out, sz)` — pending-aware statement reader.
- New `match_kw(p, stmt, kw)` — matches a control keyword that may carry an inline body;
  stashes the trailing body into `p->pending` for the next `rb_next`.
- `run_block_until` now uses `rb_next` + `match_kw` for the main read, terminator detection,
  the `then` after `if`, the `elif COND; then` chain, and `while`/`for` `do`.

Verified: `incore` and `demo-script` both pass in isolation (1/1) and in the full 20/20 run.
incore.sh was **left inline** on purpose so the parser fix is exercised end-to-end.

## maktop cannot close mak.sh0

Confirmed already protected (no code change needed): `user_shell_slot_entry` →
`shell_enter_root_tty()` sets `task_current()->unkillable = 1` before exec'ing sh.elf;
`elf_exec` reuses the same `task_t` so the flag persists; maktop kills via `sys_kill` →
`sig_deliver`, which drops kill/terminate signals on `unkillable` tasks. The signal fix above
preserves this (the `unkillable` `continue` runs before `task_terminate`).

## UI-test harness isolation (in progress — NOT fully re-verified)

Goal (per user): no cascade failures; scenarios pass **in isolation and in any order, within
the same QEMU instance, with minimal reboots**; eventually migrate off QEMU HMP `sendkey`.

### Done & verified
- **`tests/ui_runner.sh` `reset_shell` rewrite**: recover the shell **in-place first** —
  `alt-f1` (back to VT0) → `Ctrl-C` (abort/kill+reap any leftover foreground child; works now
  that the kernel restores focus on reap) → `cd /` → **probe** for a fresh `[shell:ready vt=0]`.
  If the first pass fails, a second more-forceful in-place pass. **Only if both fail** do we
  `stop_qemu`/`start_qemu` (clean relaunch) — so reboots are rare and a wedge's blast radius is
  one scenario. Guarded against `UI_REUSE_QEMU=1` (can't relaunch in that mode). Helpers added:
  `_serial_size`, `_reset_shell_inplace`. The in-place `^C` lands before each scenario's
  `start_bytes` mark, so it never pollutes a scenario's serial slice.
- **Verified with the guard in place:** full suite **20/20** (zero reboots, zero fails);
  `vt` + `bughunt` tail group **5/5** (incl. `vt-all-roundtrips`); **adversarial order 9/9**
  (`ctrlc_kills_child` mid-sequence, `vt_all_roundtrips` early, `mnt_mountpoint` disk-mutation
  interleaved) — order-independence holds.
- **The earlier `v3` freeze at `vt-all-roundtrips` (16/20) was environmental, not a
  regression**: the host slept overnight (serial log froze 00:58, resumed ~09:26), suspending
  QEMU. Re-runs of that exact scenario pass cleanly.

### Still open (lower priority)
- HMP-off migration (below) is roadmap-only, not started.

## Future: migrate UI tests off HMP `sendkey` (roadmap, not started)

HMP `sendkey` is timing-flaky under TCG (why the per-merge UI job was dropped in `a9b7474`).
Direction:
1. Lean on the in-guest driver pattern (`src/userspace/incore.sh`: run ELF, check `$?`, assert
   on serial markers — no sendkey). Move every **non-interactive** scenario in-guest.
2. Reserve keystroke injection only for genuinely **interactive** tests (Ctrl-C, arrows, VT
   switch, readline).
3. Longer term: an in-kernel event-injection path the in-guest driver controls, so pacing is
   deterministic and HMP can be retired entirely.
Stable assertion channel throughout: serial markers (`[shell:ready vt=N]`, `INCORE: …`,
`[makbox:pwd]`). See also the sh.elf POSIX-parity goal (command substitution `$(…)`, quote
stripping, globbing) — extend `docs/plans/posix-shell.md`.

## Files touched this session
- `src/kernel/include/kernel/task.h` — `task_terminate` decl
- `src/kernel/arch/i386/proc/task.c` — `task_terminate` def + `task_exit` refactor
- `src/kernel/arch/i386/proc/signal.c` — route default-terminate through `task_terminate`
- `src/userspace/sh.c` — inline-if support in the script block parser
- `tests/ui_runner.sh` — in-place `reset_shell` recovery + last-resort relaunch
- `docs/handoff-ctrlc-cascade-sh-inline-if.md` — this file
