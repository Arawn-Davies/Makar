# Handoff — branch `auth-login-installer`

Context: continuing UX/bugfix work. Nothing committed this session. Build via `./run.sh iso build`.

## DONE (in tree, unverified end-to-end unless noted)
- **mak.sh1 duplicate VT** — TOCTOU race in `vtty_register` (two makmux children claimed slot 0). Fixed with `cli/sti` atomic find-and-claim in `src/kernel/arch/i386/proc/vtty.c`. **User-confirmed fixed.**
- **Statusbar** (`src/userspace/statusbar.c`) — tabs now centre in the gap between left/right sections (no overlap with cpu/mem/rootfs); default left widget `hostname`→`command` (foreground exe). `SYS_VT_STATE` (`syscall.c`) now reports root slot (8) as focused when makmux isn't running so `command` works on mak.sh0. Installer `.sbrc` updated to `left command`. **User-confirmed fixed.**
- **ktest devfs boot-mode aware** (`src/kernel/arch/i386/proc/ktest.c`) — HDD boot failed 7/9 (empty ATAPI `/cdrom` size 0). CD asserts now gated on `devfs_node_size(cd)>0`; added `/hda` probe. Verified passing on ISO `./run.sh ktest`.
- **`sfdisk` not found on `hdd boot`** — `arawn780/gcc-cross-i686-elf:fast` image dropped util-linux/dosfstools. Added on-demand apt install in `generate-hdd.sh` and `run.sh _make_fat32_disk`. **Correct packages: `fdisk` (provides sfdisk — NOT util-linux) + `dosfstools` (mkfs.fat).** losetup already present. Verified in-container.
- **TCC incremental build** (`build-tcc.sh`) — content-hash stamp `src/userspace/.tcc.stamp` (gitignored) skips the slow tcc.c recompile when inputs unchanged. Hash covers build-tcc.sh + link.ld + crt0.o + libc.a + tinycc *.c/*.h.
- **Installer docs/src options** (`installer.c`) — new `INSTALL>components` y/N prompts (default yes) before `do_install`; `s_inst_docs`/`s_inst_src` gate `copy_tree("/docs")`/`("/src")`.
- **Installer task renamed "installer"** during wizard so statusbar `command` widget shows it — `installer_run` is now a thin wrapper around `installer_run_inner` that swaps `task_current()->name`. Added `#include <kernel/task.h>`.

## IN PROGRESS / PENDING
1. **Bleeding root shell (HIGH PRIORITY, the active task).** On the **post-install reboot only** (not a fresh generated HDD), a `root@makbox:/mnt/root#` prompt shows BEHIND the autologin (arawn) shell. **User confirmed it's STALE PIXELS, not a live task** — input correctly goes to the arawn shell. Live (pre-install) session prompt is `user@…$`. HDD boots via **Limine**. So: a transient sh.elf (defaults `g_username="root"` in `src/userspace/sh.c:53` when no `--user=`) draws a prompt at cwd `/mnt/root`, exits, and the real arawn shell paints on top without a full clear. **Fix direction (per user):** every login shell should `cd $HOME` on startup and `$HOME` set per-user (`/root` for root, `/home/<u>` else); and the session start should fully clear the framebuffer so no prior pixels bleed. Was about to inspect `vesa_tty_clear` (full-FB vs text-area) in `src/kernel/arch/i386/display/vesa_tty.c` and `shell_login_loop` clears (`shell.c` ~1226). Check whether the clear covers the whole LFB.
2. **`hdd boot` installer has no ISO9660 source** — EXPECTED: `hdd boot` attaches no CD. My edit to attach `makar.iso` as cdrom in run.sh `hdd boot` was **REJECTED by user** — do NOT re-apply without asking. The installer code itself is unbroken (my installer.c diff doesn't touch `find_cdrom`/`g_cd`/iso9660). User may want a different approach.
3. Lower priority: statusbar `installer` display (#11, done but unverified), docs/src options (#12, done but unverified).

## NOTES
- Stray headless QEMU probes may be running against `/tmp/hdd-probe.img` (a copy, not `hdd.img`). Kill if found.
- No AI attribution in commits/PRs (project rule). One commit per work item.
- Framebuffer pixels are not asserted in tests; coverage is serial-marker based (in-guest script drivers + the key-injection harness). Visual checks are manual via a windowed run (`./run.sh ... gui`).
