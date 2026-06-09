# Makar — progress

The current branch's uncommitted work, split into **two PRs**, plus the backlog.
Tasks are incrementally numbered T1…TN. Generated 2026-06-09.

> Earlier work (250 Hz scheduler, ANSI/VT100 terminal, the graphical installer,
> fork/exec/wait, the page cache, the `mx*` GUI suite) already shipped in prior
> merged PRs and isn't re-listed here.

---

## PR 1 — GPU / video framework + display robustness

The `feat/gpu-video-framework` namesake: the display-driver layer and the
fail-safe panic / bounded-wait hardening.

- [x] **T1** — Video-driver vtable + VBE default backend + present routing (`kernel/video.h`)
- [x] **T2** — VMware/VirtualBox SVGA II backend: 2D accel + hardware cursor, CI-tested
- [x] **T3** — Fix VMware/VBox mouse (dual-path SVGA cursor) + the 1080p mode-switch crash
- [x] **T4** — Default boot resolution 720p + `RES=` flag in `run.sh` (→ `vmode=`)
- [x] **T5** — Bootloader-selectable resolution (`vmode=`, the resolution submenu)
- [x] **T6** — Display-settings app `mxdisplay` (resolution change + confirm/auto-revert)
- [x] **T7** — Shared i8042/PS2 controller module (mouse independent of keyboard)
- [x] **T8** — Fail-safe panic: VGA-text panic screen, bounded device-wait spins, page-table-pool + FB-validation hardening, `Ctrl+Alt+Shift+P` debug chord

## PR 2 — Boot experience + GUI desktop polish + docs

The XP/Vista boot overhaul, the desktop tray, and the documentation pass.

- [x] **T9** — GUI-first boot: hide the console, raise the graphical splash at the earliest stable point, ~5 s cosmetic dwell
- [x] **T10** — `sysadmin` (deferred drivers + `go32` resume) + `hwspecs` hardware-info text modes
- [x] **T11** — Bootloader menu reorg: GUI desktop at top + everything else under **Advanced options** (GRUB ×2 + Limine)
- [x] **T12** — Net / clock / date tray → the bottom dock (one place; power button stays top-right)
- [x] **T13** — Graphical boot splash (green-leaf emblem + wordmark + loading bar) + new logo/branding
- [x] **T14** — Fix: GUI boot no longer shows the classic ASCII loading bar (hands straight to the splash)
- [x] **T15** — Fix: `sysadmin` / `hwspecs` / rescue force a real VGA text console (no VESA)
- [x] **T16** — Conventions doc (`docs/conventions.md`) + repo-wide docs & roadmap audit
- [x] **T17** — Per-module docs sweep: 7 new `docs/kernel/*.md` (video/mouse/vm/fpu/bochs_vbe/pci/acpi) + existing-doc drift fixes (debug panic, vesa vtable+splash, paging 32→128, procfs `/proc/rtc`, keyboard i8042/chord, system→debug xref)
- [x] **T18** — Screenshots of the GUI + boot modes → `images/` + a README gallery, captured by `tools/capture-screens.sh`
- [ ] **T19** — Build, `copy.sh`, commit, push, update the PR body

---

## Backlog (future PRs)

- [ ] **T19** — AHCI (SATA) + a `blkdev` vtable
- [ ] **T20** — USB HID keyboard + mouse (UHCI → xHCI)
- [ ] **T21** — UEFI boot survivability (OVMF)
- [ ] **T22** — Hyper-V synthvid (VMBus) display backend
- [ ] **T23** — `mxweb` — HTTP over lwIP + a minimal HTML renderer
- [ ] **T24** — `mximg` PNG + JPEG decode (vendor stb_image / inflate)
- [ ] **T25** — Real `.ico` desktop icons in `/usr/share/img` + loader
- [ ] **T26** — Busy mouse cursor (hourglass/beachball) during heavy ops
- [ ] **T27** — GUI responsiveness: fix one-event click lag / double-click
- [ ] **T28** — File-path boxes become text-entry boxes (mximg, editor, Files)
- [ ] **T29** — Windowed framebuffer for console-launched graphical apps (Doom in a makx window)
- [ ] **T30** — Doom IWAD/PWAD selection launcher
- [ ] **T31** — Desktop wallpaper: "set as background" from the image app + WM stretch
- [ ] **T32** — GPU-usage stat on the dock tray
- [ ] **T33** — `~/.mxrc` per-user GUI profile (tray toggles + wallpaper; `/home/user/.mxrc` on the livecd)
- [ ] **T34** — Memory management continuation (PMM accounting + reclaim/GC tuning)
- [ ] **T35** — Self-hosted i686-makar toolchain → GHCR (parked)
- [ ] **T36** — Linux-ification: cut the ~101-syscall surface toward UNIX idioms (device files, ioctl, getdents)
