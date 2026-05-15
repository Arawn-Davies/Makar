# Shell — historical WIP note (archived)

> **Status:** ✅ Shipped on main. This note is kept for git-blame
> context only; the live reference is [`docs/kernel/shell.md`](../docs/kernel/shell.md).

The shell described here landed long before the multi-TTY refactor.
Everything in the original checklist is now implemented and surpassed:

- Inline editing, history (↑/↓ + `!!`), Ctrl+C sigint
- Module-table dispatch with a `fullscreen` flag for FB-restoring builtins
- VFS-aware tab completion across `/hd`, `/cdrom`, `/proc`, and the
  virtual root
- Glob expansion (`*`, `?`) on argv via `shell_glob.c`
- PATH lookup for ELFs (`/cdrom/apps/`, `/hd/apps/`)
- `exec <path>` plus 5 builtins that paint over the framebuffer
- Per-TTY shells (`shell0`–`shell3`) running as preemptive kernel tasks

The renamed VIX editor (was VICS — see `docs/makar-medli.md`) is
launched via the `vix` builtin.

See [`docs/kernel/shell.md`](../docs/kernel/shell.md) for the current
implementation reference and [`SURVEY.md`](../SURVEY.md) for the full
command inventory.
