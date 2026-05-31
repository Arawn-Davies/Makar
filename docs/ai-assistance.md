---
title: AI assistance
parent: About
nav_order: 2
---

# AI assistance

A short note on how LLMs are used in this project, because they show
up in the commit history and I'd rather be upfront about it than have
people guess.

## TL;DR

LLMs are a power tool here — boilerplate generator, rubber duck,
search engine with context. They are not the author. Every design
decision, every architectural choice, every "why is it shaped this
way" answer comes from me. If you ask me on the spot how the
scheduler reaper handles a dead task's page directory or why
`shell_exec_elf` stashes `exec_params` on `task_t` instead of a
shared global, I can tell you, because I designed it and debugged
it. The LLM helped me type it faster.

## Background

I've been writing OS code long enough to know what I'm doing.
[Medli](https://github.com/Arawn-Davies/Medli) is my previous OS
project — a managed-code (COSMOS / C#) hobby OS that I built from
the ground up. Makar is the bare-metal C/asm successor: same
designer, different language, different goals (closer to Linux ABI,
real ring-3, real preemption, eventual self-hosting).

That history matters because it sets the bar for what counts as
"understanding". I'm not learning paging from an LLM. I'm not
asking it what an IDT is. The kernel internals — GDT/IDT layout,
paging at 4 MiB and 4 KiB granularity, ring transitions via `iret`,
the Multiboot 2 handoff, FAT32 cluster chains, IDE PIO — all of
that lives in my head, not in a chat window.

## What LLMs are good for here

- **Typing speed on well-understood code.** Once I've decided that
  `shell_exec_elf` needs a per-task heap-allocated params struct,
  writing the actual `kmalloc` / `strncpy` / `kfree` plumbing is
  mechanical. An LLM can produce that in seconds with a clear
  enough prompt.
- **Tedious surface work.** Doc rewrites, changelog formatting,
  shell-test scaffolding, GitHub issue cleanup, renaming 20 issues
  to a consistent convention — these are the kinds of jobs where
  spending half an hour by hand buys nothing. Delegate, review,
  ship.
- **Rubber-ducking a known-shape bug.** When the symptom is
  "ring-3 jumps to `CS=0x3F8`" and I already suspect a races-and-
  globals problem in the exec path, talking through the suspect
  code path with an LLM is faster than staring at it alone. The
  fix is still mine to choose.
- **Cross-checking my reading of a spec.** "Does this paragraph of
  the FAT32 spec mean what I think it means" is a legitimate use of
  a fast search-with-context tool.

## What LLMs are *not* doing on this project

- **They are not picking the architecture.** Single-kernel two-ISO,
  per-task fd table, deferred PD reaping, layered keyboard decoder,
  per-TTY backing grids with deferred FB repaint — every one of
  those shapes is mine. I can tell you why I picked each.
- **They are not deciding what ships.** PR scope, slice boundaries,
  what goes in vs. what waits for a follow-up, what counts as
  "done enough to merge" — all me.
- **They are not driving the testing strategy.** The ktest harness,
  the GDB boot-checkpoint groups, the UI-test shared-VM runner with
  its `sendkey` pacing, the decision to run `iso-test` + `ui-test`
  before every PR — designed and tuned by me, based on which
  classes of regression have actually bitten this project.
- **They are not the source of truth on the codebase.** When I ask
  an LLM about Makar, it's reading my files just like I would.
  When it's wrong, I catch it — because I wrote the thing it's
  describing.

## How a typical session goes

1. I decide what I want to build, often by reading the existing
   code, sketching on paper, and weighing trade-offs against the
   roadmap.
2. I describe the change to the LLM with the precise files and
   constraints involved. I do *not* ask it to "figure out what to
   do" — I ask it to do a specific, scoped thing.
3. It produces a diff. I read every line. If it's wrong, I push
   back; if the design is wrong but the code matches my
   description, that's on me for describing it wrong.
4. I run `iso-test` and `ui-test`. If something regresses, I
   debug it. Sometimes the LLM helps me think through the trace;
   the diagnosis is still mine to make.
5. I commit in my own voice, with my own attribution. No
   `Co-Authored-By: Claude` trailers, no "Generated with Claude
   Code" footers. The code is mine to maintain, so it ships under
   my name.

## LLMs in the wider OSDev scene

Makar is not unusual in this. By 2026, LLM assistance is openly
common across the hobby OS landscape — the question has shifted
from "is anyone using these" to "how honestly are they being used".

A few patterns I've seen in other projects:

- **Driver and boilerplate scaffolding.** Many hobby kernels openly
  credit LLMs for first drafts of device drivers (PCI enumeration,
  AHCI, virtio-net), especially when the spec is verbose and the
  shape of the code is dictated by the hardware rather than by
  design. The author still has to debug it on real or emulated
  hardware, which is where understanding becomes non-negotiable.
- **Cross-compiler and toolchain setup.** OSDev wiki walkthroughs
  have been a go-to LLM reference for years; the LLM essentially
  re-serves the wiki with project-specific context. Useful, but no
  substitute for understanding what `-ffreestanding`,
  `-nostdlib`, and a custom linker script actually do.
- **Spec-bashing.** Reading the Intel SDM, the Multiboot spec, the
  FAT32 white paper, the ELF ABI — long documents where the LLM is
  a faster index than ctrl-F. Same caveat: the answer needs
  cross-checking against the actual document, because LLMs
  hallucinate plausible-looking bit positions.
- **The "vibe-coded kernel" anti-pattern.** The cautionary tail of
  the same trend: projects where the author can't explain their
  own GDT, where the codebase is a collage of LLM outputs that
  individually compile but collectively don't cohere. These tend
  to stall the first time a real bug appears, because debugging
  requires a model of the system that nobody on the team actually
  has. The give-away is usually a README that describes ambitious
  features and a commit history that's all "fix from AI suggestion".

OSDev sits in a useful spot for LLMs: well-documented public specs,
small self-contained projects, decades of prior-art on the wiki and
in Linux/BSD source. That makes the assistance genuinely useful for
people who already understand the field. It also makes it easy to
generate plausible-looking nonsense for people who don't. Both
ends of that spectrum exist in public hobby OS work today; this
project tries to be honest about which end it sits on.

## Why I'm writing this down

Open-source hobby OS projects attract scrutiny, and "did an AI
write this" is a fair question to ask in 2026. The honest answer
is: I drove every design decision, I understand every subsystem,
I can defend every trade-off — and yes, I used LLMs as a fast
typewriter and a 24/7 rubber duck. Both things are true. Treating
that as embarrassing would be silly; treating it as "the AI built
my OS" would be a lie.

If something in this repo doesn't make sense, open an issue.
You'll get an answer from me, not from a model.
