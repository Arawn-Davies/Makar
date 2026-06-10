# doomgeneric-makar

The **Makar platform backend for [doomgeneric](https://github.com/ozkl/doomgeneric)** —
the glue that turns vendored doomgeneric into Makar's `doom.elf`.

`doomgeneric_makar.c` implements the `DG_*` platform hooks (init, draw-frame,
sleep, ticks, get-key, set-title) on Makar's syscall ABI: it presents into a
**makx** window when launched with `-makx <server-pid>` (windowed client over
shared surfaces + IPC) and falls back to a full-screen framebuffer otherwise. It
also locates the IWAD on the XFCE-style asset path `/usr/share/games/doom/`
(FreeDOOM preferred).

It is deliberately a **self-contained folder** so it can move to its own repo
later, alongside sibling ports built the same way — e.g. a future
`quakegeneric-makar` (the Makar backend for
[quakegeneric](https://github.com/erysdren/quakegeneric)) and other id-tech /
90s DOS game ports. Each such port pairs one `*generic_makar.c` backend with a
vendored upstream engine and links against the Makar hosted libc + `makx`.

## Build

Built by `src/userspace/Makefile` (`DGM_DIR=doomgeneric-makar`): the upstream
doomgeneric sources come from `vendor/doomgeneric/doomgeneric`, this backend is
compiled alongside them, and they link into `doom.elf`. The launcher GUI
(`mxdoom.elf`, `src/userspace/mxdoom.c`) stays part of the OS — it forks
`doom.elf` with the chosen IWAD/PWAD/skill/map args — and is **not** part of this
port folder.

Upstream doomgeneric is GPLv2 (see `CLAUDE.history.md` / `LICENSES/`); this
backend inherits that licence.
