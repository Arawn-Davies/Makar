# Plan: move the kernel shell to userspace

## Summary

Move Makar's normal interactive shell out of the kernel and into
`/apps/sh.elf`.  The userspace shell should become the default shell on
all VTs, while the kernel keeps only a tiny rescue shell for boot/debug
fallback.

This is not a continuation of the small parallel `src/userspace/sh.c`
implementation.  The implementation should port the existing kernel
shell behavior into userspace so features are not reinvented piecemeal.

Admin command names should remain available from `sh.elf`.  Privileged
behavior should move behind explicit kernel syscalls with a shared
privilege check, so future login/sudo work can authorize the same
operations without reintroducing a kernel shell dependency.

## Goals

- `/apps/sh.elf` is the normal shell executable.
- Normal boot starts one userspace login shell per VT.
- Kernel shell becomes a minimal rescue shell only.
- Userspace shell keeps the current kernel-shell user experience:
  readline, history, tab completion, globbing, PATH, makbox fallback,
  scripting, and core builtins.
- Admin commands remain visible in userspace shell, backed by privileged
  syscalls where implemented.
- No long-lived shared shell core is required.  Port the kernel shell
  logic into userspace and let the rescue shell shrink separately.

## Current Source Shape

- Normal boot currently creates four kernel shell tasks in
  `src/kernel/kernel/kernel.c` with `task_create("shellN", shell_run)`.
- VT ownership is assigned by `vtty_register()`, currently called from
  `shell_run()`.
- `vtty_switch()` finds the slot owner by looking for the live task with
  the lowest pid for that `task_t.tty`.
- `SYS_EXECVE` already transfers keyboard focus and VT foreground to
  the exec'd task, so a ring-3 shell can launch children using
  `fork`/`execve`/`wait4`.
- Existing userspace `sh.elf` already has some shell behavior, but it is
  smaller than the kernel shell and should be replaced or heavily
  rewritten by the port.

## Implementation Plan

### 1. Add a userspace login-shell boot path

Add a kernel task entry, for example `user_shell_slot_entry`, that:

1. Calls `vtty_register()` to claim the next VT slot.
2. Applies the same initial VT palette/status setup that `shell_run()`
   currently performs.
3. Calls `elf_exec("/apps/sh.elf", 2, argv)` with:
   - `argv[0] = "sh.elf"`
   - `argv[1] = "--login"`
4. Falls back to the rescue shell if `elf_exec()` returns.

Change normal boot to create four of these tasks instead of four full
kernel shell tasks.

Keep `ktest_bg_task` behavior unchanged.

### 2. Add boot fallback selection

Add a Multiboot command-line flag:

```text
shell=rescue
```

When present, boot the tiny kernel rescue shell on each VT instead of
the userspace shell.  This gives developers a recovery path if
`/apps/sh.elf`, the rootfs, or shell syscalls regress.

Also fall back to the rescue shell automatically per VT if
`/apps/sh.elf` cannot be read or exec'd.

### 3. Port kernel shell behavior into `/apps/sh.elf`

Replace the current small `src/userspace/sh.c` implementation with a
ported userspace shell.

The port should preserve these kernel-shell behaviors:

- Prompt/readline with inline editing.
- 16-entry history.
- Left/right/up/down handling.
- Zsh-style tab completion and cycling.
- PATH lookup with default `/apps`.
- `.elf` suffix probing.
- Restricted makbox fallback for known applets:
  `ls`, `cat`, `cp`, `mv`, `rm`, `rmdir`, `echo`, `pwd`.
- Glob expansion for `*` and `?`.
- Variables and expansion:
  `NAME=value`, `$VAR`, `${VAR}`, `$?`.
- Builtins:
  `cd`, `pwd`, `exit`, `env`, `unset`, `read`, `sleep`, `true`,
  `false`, `[ ... ]`.
- Script execution:
  `sh <file>`, comments, semicolon splitting, `if`/`elif`/`else`/`fi`,
  `while`, `for ... in`, and status propagation through `$?`.

`/apps/sh.elf --login` behavior:

- Owns a VT.
- Does not terminate on `exit` or Ctrl-D.
- Prints a short message and reprompts instead.

Plain `/apps/sh.elf` behavior:

- Nested/manual shell.
- Exits normally on `exit` or Ctrl-D.

### 4. Keep admin command names in userspace

Admin command names should remain available in `sh.elf`, not reserved
for the rescue shell.

Implement admin commands through explicit syscalls where practical.
Each syscall should call a shared privilege check such as:

```c
int task_is_admin(task_t *t);
```

Initial policy:

- Makar has no real user model yet.
- All boot/session tasks are treated as admin/root for now.
- The check must still exist so future login/sudo work can change one
  policy point instead of rewriting shell command paths.

Minimum admin command syscall targets:

- `mount`
- `umount`
- `mkfs.ext2`
- `mkfs.fat32`
- `setmode`
- `fgcol`
- `bgcol`
- `eject`
- `shutdown`
- `reboot`
- `sched_quantum`

Commands that may remain visible stubs in the first implementation if
wrapping them cleanly would make the PR too large:

- `install`
- `chainload`
- `readsector`
- partition editing helpers such as `mkpart` / `lspart`
- `ktest`

Stub text should be clear and stable, for example:

```text
install: not available from userspace yet
```

Do not implement an opaque "kernel shell proxy" for admin commands.
Use explicit syscalls so the authorization surface is auditable.

### 5. Shrink kernel shell to rescue shell

After the userspace shell boots and passes parity tests, reduce the
kernel shell to rescue scope.

Keep only enough to recover:

- `help`
- `exec /apps/sh.elf`
- `ls`
- `cat`
- `pwd`
- `cd`
- `verbose`
- `reboot`
- `shutdown`

The rescue shell does not need full scripting, completion, globbing,
admin command parity, or normal-user polish.

## Interfaces

### Executable

```text
/apps/sh.elf [--login]
```

`--login` means this is a VT owner shell and should not exit from user
input.

### Kernel command line

```text
shell=rescue
```

Forces kernel rescue shell startup instead of userspace shell startup.

### Privilege check

Add one shared kernel-side authorization point for privileged shell
syscalls:

```c
int task_is_admin(task_t *t);
```

Default implementation returns true for all current tasks until a real
user/login model exists.

## Testing Plan

### Build checks

Run:

```sh
./run.sh iso build
```

### UI shell parity

Update existing UI tests so normal shell scenarios start at the
userspace shell prompt rather than typing `exec /apps/sh.elf` from the
kernel shell.

Add or update scenarios for:

- Boot reaches userspace shell prompt on VT0.
- Alt+F1-F4 switches among four userspace shells.
- `exit` in `--login` shell does not kill the VT.
- Ctrl-D in `--login` shell does not kill the VT.
- Nested `/apps/sh.elf` exits normally.
- `cd`, `pwd`, PATH lookup, `.elf` probing.
- Makbox fallback applets.
- Tab completion and tab cycling.
- Glob expansion.
- Variables, `$?`, `env`, `unset`, `read`.
- `sh /apps/demo.sh` or equivalent script coverage.
- `if`/`elif`/`else`/`fi`, `while`, `for`, `[ ... ]`,
  `true`, `false`, `sleep`.

### Admin command coverage

From `sh.elf`, test safe admin command paths:

- `setmode`
- `fgcol` / `bgcol`
- `sched_quantum`
- `mount` / `umount` on the scratch disk
- `mkfs.ext2` on the scratch disk
- `mkfs.fat32` where supported by the existing disk size/path
- `reboot` / `shutdown` only in scenarios designed to terminate QEMU

For visible stubs, assert the stable "not available from userspace yet"
message.

### Rescue coverage

Add scenarios for:

- `shell=rescue` boots the rescue shell.
- Missing or unreadable `/apps/sh.elf` falls back to rescue shell.
- Rescue shell can run `exec /apps/sh.elf` manually after recovery.

### Regression sweep

Run at least:

```sh
./run.sh ui shell posix fs
./run.sh ui
```

## Acceptance Criteria

- Normal boot presents userspace shell prompts on all four VTs.
- Kernel shell is no longer the normal interactive shell.
- Existing shell/script workflows still work from `sh.elf`.
- Admin command names are present in `sh.elf`.
- Implemented admin commands go through explicit privileged syscalls.
- Unsupported admin commands fail with clear userspace-shell messages,
  not "unknown command".
- `shell=rescue` and automatic exec failure fallback both work.

## Notes for Future Sudo/Login Work

Do not special-case sudo in the shell command implementations.  The
shell should call the same admin syscalls regardless of whether the
caller is root, sudo-elevated, or unprivileged.

Future work should add credentials/capabilities to `task_t` and update
`task_is_admin()` accordingly.  That preserves one enforcement point for
all admin shell commands.
