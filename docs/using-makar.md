---
title: Using Makar
nav_order: 3
has_children: true
---

# Using Makar

This section covers day-to-day use inside the running OS: shells, scripting,
the BASIC interpreter, and the in-OS kernel rebuild workflow.

| Guide | What it covers |
|---|---|
| [Shell scripting](scripting.md) | The split between `/apps/sh.elf` and the in-kernel script runner, including variables, tests, control flow, userspace shell features, and limitations. |
| [BASIC](basic.md) | The `basic.elf` interpreter, REPL commands, line-numbered programs, integer expressions, graphics statements, and bundled samples. |
| [Rebuild kernel](rebuild-kernel.md) | Building the kernel from inside Makar, including prerequisites and workflow notes. |

For host-side build, boot, and test commands, use
[Building and running](building.md). For implementation details behind the
user-facing behavior, use [Internals](internals.md) and the
[Kernel subsystems](kernel/) reference.
