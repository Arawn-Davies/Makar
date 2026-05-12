# Shell - interactive kernel command loop

> **Status:** complete (ongoing: new commands added with each feature)
> **Branch:** landed on `main`

## Summary

Minimal interactive shell reading PS/2 keyboard input, dispatching built-in
commands, and executing ELF userspace apps from the VFS.

## Implemented

- [x] Line editor with echo, backspace, arrow-key history, Ctrl+C cancel
- [x] Command dispatch via registry table (`shell_cmds.c` modules)
- [x] Welcome banner and version string at boot
- [x] Prompt matching Medli UX: `user@makar /path> `
- [x] VFS-aware commands: `ls`, `cd`, `cat`, `mount`, `lspart`, `mkpart`
- [x] Disk commands: `lsdisks`, `readsector`
- [x] System commands: `help`, `clear`, `echo`, `meminfo`, `uptime`, `shutdown`
- [x] Debug/test commands: `ktest`, `ring3test`, `vicstest`, `splitscreen`
- [x] ELF userspace execution: `exec <path>` launches ring-3 ELF binaries
- [x] Userspace apps: `hello`, `calc` (bc-style expression evaluator)

## Source

- `src/kernel/arch/i386/shell/`
- `docs/kernel/shell.md`
