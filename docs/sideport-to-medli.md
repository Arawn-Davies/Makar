---
title: Sideporting to Medli
parent: Development
nav_order: 4
---

# Sideporting Makar features to Medli

**"Sideporting"** — the cousin of backporting: instead of carrying a fix
across versions, we carry a feature *sideways* between sibling projects
that share the same UX target but live on different stacks.

- [Makar](https://github.com/Arawn-Davies/makar) — bare-metal C on
  i686-elf, GRUB Multiboot 2, manual paging/IRQs.
- [Medli](https://github.com/Arawn-Davies/Medli) — C# on
  [Cosmos](https://github.com/CosmosOS/Cosmos), which uses IL2CPU to
  compile CIL → x86 and owns the boot, kernel, GC, IRQs, and memory.

The two never share binaries.  What they *can* share is **behaviour,
vocabulary, and visible UX** — the things a user notices first.  Each
candidate below is rated on how cleanly it crosses the
C↔C#/managed-kernel boundary.

---

## Sideport scoring rubric

| Rating | Meaning |
|---|---|
| Easy | Logic is pure data-shuffling. Translate idiom-for-idiom in C#. |
| Moderate | Needs a Cosmos / IL2CPU equivalent (timer, IRQ hook, ATA) that already exists in Cosmos. |
| Hard | Requires a Cosmos plugin or extending IL2CPU. Doable, but a real subproject. |
| Blocked | Tied to bare-metal mechanics Cosmos owns (paging, ring switches, custom GDT/IDT). Not portable; reinvent the *outcome* differently. |

---

## High-value candidates

### 1. Shell UX — Easy

Makar shell features that are pure string-manipulation + line-buffer
state and would translate directly to Medli's existing C# shell:

- **Inline editing** (cursor movement, insert at point) — `src/kernel/arch/i386/shell/shell.c`.
- **History ring + Up/Down navigation** + `!!` recall (16 entries, echo-then-run).
- **Tab completion** — two-phase (command name first, then path) with the unique-match space, ambiguity-list, and visible-feedback semantics from `feat/vics-vim-polish` (#130).
- **Wildcard glob expansion** across VFS (#129) — `src/kernel/arch/i386/shell/shell_glob.c`. Pattern is `*`/`?`/`[…]` against a directory listing; trivially expressible in C# using `Directory.EnumerateFileSystemEntries`-style enumeration over the Cosmos VFS.
- **Prompt format** — `root@<host> <cwd>~> ` (with the distinctive `~>` suffix that doubles as a reliable scrape marker).
- **`Ctrl+C` abort-input** semantics (clear line, print `^C`, redraw prompt).

These are pure ergonomic ports.  Pick one feature per PR; each lands as
~50-200 lines of C#.

### 2. Command vocabulary — Easy

Aligning the two shells on the same builtins makes the projects feel
like one OS.  Makar today has: `ls`, `cd`, `cat`, `mkdir`, `rm`,
`rmdir`, `mv`, `cp`, `mount`, `umount`, `clear`, `uptime`, `setmode`,
`lsman`, `man`, `exec`, `ktest`, `diskinfo`.

Cross-check against Medli's command set; for each that exists on both,
pick the Makar name as canonical (because it tracks
[POSIX](https://pubs.opengroup.org/onlinepubs/9699919799/)).  Names
that diverge today (Medli's DOS-ish path separator is the canonical
example) should converge towards the Makar/Unix spelling at the *shell*
layer, even if storage stays DOS-style underneath.

### 3. `/proc` synthetic filesystem — Easy

Makar's `/proc` (#129) is just an enum-dispatched switch:
`cpuinfo`, `meminfo`, `tasks`, `uname`, generated on demand by
`procfs_read_file()`.  No on-disk state.

In Medli this is a `VirtualFolder`-style class that returns a `byte[]`
per entry, computed from the Cosmos kernel's existing CPU/memory/task
APIs.  Filesystem readers (`cat`, `more`) see it as a normal file.
The exact `cpuinfo` field layout (vendor_id, model name, flags) is
worth keeping byte-compatible with Linux so any tooling that scrapes
either OS gets the same fields.

### 4. VIX editor UX — Moderate

VIX (Makar's vi-style editor, ELKS/FUZIX lineage) is already
philosophically Medli's `vics`.  The *behavior* is portable:

- Pane-derived terminal size (`vesa_pane_t` ↔ whatever Medli's TTY
  abstraction is).
- Modal: normal / insert / command-line.
- Movement: `h j k l`, `w b`, `0 $`, `gg G`.
- Persistent on-screen `:` command line that knows about `:w`, `:q`,
  `:wq`, `:q!`.
- Optional vim-polish behaviours from #130.

What does *not* port: Makar's `SYS_PUTCH_AT` / `SYS_SET_CURSOR` syscalls
and the ring-3 ELF model.  Medli runs the editor in-process under
Cosmos, so it can call the Cosmos terminal API directly — simpler, not
harder.

### 5. Layered keyboard model — Moderate

The slice 5/5b layered driver — **scancode → keycode → ASCII/sentinel
→ per-TTY ring** — is genuinely a good design and Medli would benefit.
The C code (`src/kernel/arch/i386/drivers/keyboard.c`) is reference
material, not a literal port.  What matters in Medli:

- Decoder owns modifier state; consumers see only logical events.
- Sentinel bytes for non-ASCII keys (arrows, F-keys, modifier change)
  use the high half of the byte range so an `unsigned char` cast can't
  sign-extend them into negative ints.  Pin the exact range
  (`0x80`-`0x8F`) so future userspace tooling sees identical events on
  both OSes.
- IRQ handler enqueues, consumer dequeues — single-producer / single-
  consumer ring per TTY.

Cosmos already owns the PS/2 IRQ in C#; the port is "rewrite the
decoder as a C# state machine that emits the same sentinel events."

### 6. Loading-screen + boot-time ktest pattern — Moderate

The "loading spinner ticks alongside background self-tests; bar only
advances after each suite passes; FAIL→panic, PASS→silent" pattern
(#128) is a generally good boot UX.  The Medli equivalent is a Cosmos
boot screen running self-checks against the Cosmos kernel's own
internals (heap, GC handle table, file-system mount status).  Same
*shape*, different checks.

---

## Things that can't be sideported (and why)

### Ring-3 ELF userspace — Blocked

Makar's `iret` to user mode, per-task page directory, ELF32 loader,
`int 0x80` syscall ABI, and `crt0.S`/`link.ld` toolchain are all tied
to running un-managed code on real ring transitions.  Cosmos owns the
ring transitions and runs everything as managed CIL after IL2CPU.  The
equivalent in Medli is **AppDomain-like isolation between managed
assemblies**, not ring 3.

Don't try to sideport `exec(2)` — design Medli's app-launch UX to look
identical (`exec <name>`, argv, exit status) but back it with managed
assembly loading.

### Per-task page directory + reaper — Blocked

The slice 1 reaper pattern (defer-free a dead task's user PD until CR3
has switched away) is specific to manual paging.  Cosmos's GC handles
the equivalent reclamation problem differently — no port needed.

### `SYS_BRK` / `SYS_MMAP` / manual heap extension — Blocked

The libc-porting work in `docs/userland-libc.md` exists because Makar
needs to feed musl/uClibc-ng a real syscall surface.  Medli has the
.NET BCL — `String`, `Stream`, `Dictionary` are already there.  The
sideport here is *not* a syscall but **shared semantics**: if Makar
ships a userland `ls` whose argv parsing matches a Medli `ls`, that's
the win.

### GRUB Multiboot 2 boot path — Blocked

Cosmos builds its own bootloader.  No sideport.

---

## Things to sideport *from* Medli *into* Makar

Sideporting goes both ways.  Medli is older and has features Makar
hasn't built yet:

- **User account system** — Makar deferred this until signals + per-task
  FD table landed (#134 / this PR closed both).  Medli's account model
  is the reference: log-in prompt, per-user home dir, prompt
  user-segment (`<user>@<host>`).  Makar already echoes `root@makar` —
  the structure is there, just no second user.
- **Daemon / service model** — Medli has `Daemon` objects with
  registration/start/stop.  Makar currently has bare preemptive kernel
  tasks; promoting them to "named services with a status/restart
  command" is the obvious port.
- **Anything in Medli's docs that has already worked through "what does
  a user expect here?"** for shared territory (mount UX, error
  message wording, history behaviour quirks).

---

## Recommended first sideport

If picking one to start: **wildcard glob + tab completion semantics**
(#130 in Makar).  Reasons:

1. Pure logic — no Cosmos plugin needed.
2. Immediately visible to anyone using either shell.
3. The Makar test surface (in-guest drivers shell-smoke.sh/incore.sh,
   `tab_complete_path`) is a literal spec — a Medli C# port can mirror
   the same assertions against its own shell harness, giving both
   projects a single behaviour contract that's been wire-tested.

---

## Process

Each sideport should be one PR per side, both linking to a single
tracking issue ("Sideport: wildcard glob").  The PR description on each
side cites the other.  Avoid "shared code submodule" or any compile-
time coupling — the projects gain more from disciplined parallelism
than from forced sharing.

## See also

- [Makar × Medli — Co-operation roadmap](makar-medli.html) — broader
  positioning of the two projects.
- [Userland libc porting](userland-libc.html) — Makar-specific syscall
  work that frees the shell to move userside.
