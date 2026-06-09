# Makar — progress

Tasks are incrementally numbered T1…TN, split into **Done** and **To do**.
Last reordered 2026-06-09.

> Earlier work (250 Hz scheduler, ANSI/VT100 terminal, the graphical installer,
> fork/exec/wait, the page cache, the `mx*` GUI suite) already shipped in prior
> merged PRs and isn't re-listed here.

---

## Done

### PR 1 — GPU / video framework + display robustness (merged)

- [x] **T1** — Video-driver vtable + VBE default backend + present routing (`kernel/video.h`)
- [x] **T2** — VMware/VirtualBox SVGA II backend: 2D accel + hardware cursor, CI-tested
- [x] **T3** — Fix VMware/VBox mouse (dual-path SVGA cursor) + the 1080p mode-switch crash
- [x] **T4** — Default boot resolution 720p + `RES=` flag in `run.sh` (→ `vmode=`)
- [x] **T5** — Bootloader-selectable resolution (`vmode=`, the resolution submenu)
- [x] **T6** — Display-settings app `mxdisplay` (resolution change + confirm/auto-revert)
- [x] **T7** — Shared i8042/PS2 controller module (mouse independent of keyboard)
- [x] **T8** — Fail-safe panic: VGA-text panic screen, bounded device-wait spins, page-table-pool + FB-validation hardening, `Ctrl+Alt+Shift+P` debug chord

### PR 2 — Boot experience + GUI desktop polish + docs (0.10.5, merged)

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
- [x] **T19** — Build, `copy.sh`, commit, push, update the PR body (shipped as the 0.10.5 release PR #202)

### Image decoders + desktop overhaul (branch `feat/mximg-png`)

- [x] **T25** — `mximg` PNG decode — shared `img_png.c` (from-scratch RFC1951 inflate + all 5 scanline filters, bit depths 1-16, colour types 0/2/3/4/6, non-interlaced); host-validated byte-for-byte + decodes the shipped `/usr/share/pixmaps` logos. JPEG split out to T38.
- [x] **T26** — Real `.ico` desktop icons + shared loader (`img_ico.c`: BMP-DIB 32/24/8/4/1-bit + PNG-embedded entries, best-size pick; host-validated). WM loads `.ico`/`.png`/`.bmp`; `tools/bmp2ico.py` generates `data/icons/*.ico` shipped to `/usr/share/icons/makar`.
- [x] **T39** — XFCE-style `.desktop` shortcuts: system-wide `/usr/share/shortcuts` + user overlay `~/.shortcuts` (overrides by filename); WM parses Name/Icon/Exec + `X-Makar-*` extensions (built-in default set as fallback)
- [x] **T40** — Draggable + selectable desktop icons (click selects/highlights + hover lift; a no-move click launches; a drag drops and persists `X-Makar-IconX/Y` back into the source `.desktop`, best-effort)
- [x] **T41** — Real DOOM desktop logo: `M_DOOM` lump extracted from `DOOM1.WAD` (DOOM patch format + `PLAYPAL`) rendered onto the icon tile
- [x] **T32** — Desktop wallpaper (X11 root-pixmap style): `mximg` "Set Wallpaper" (1) persists the path to `~/.mxrc` (`Wallpaper=`, reloaded at next boot via `load_image_any`) and (2) hands the WM the decoded pixels **now** as a shared surface over `MX_WALLPAPER` (`sid`/`w`/`h`) — applied instantly, no cross-process file read (which the page cache made unreliable). The WM blits it stretched behind the icons (flat `COL_DESK` when unset); a ~1s `.mxrc` poll covers external edits. `bmp_load_max` lifts the icon-sized 256px cap for wallpaper BMPs. guitest asserts a staged `~/.mxrc` wallpaper renders (`GUITEST=1` + `tests/guitest_wallpaper_check.py`). (Introduces the `~/.mxrc` per-user profile — T34 groundwork.)
- [x] **T27** — Busy mouse cursor: an hourglass sprite replaces the arrow while a launched client has no surface yet (`wm_busy()`); swaps the HW-cursor sprite on the accelerated path, software bitmap otherwise
- [x] **T29** — File-path text-entry box in the shared file dialog (`gui_browser`): editable path field replaces the static cwd label; typing a directory + Enter jumps there, a file + Enter opens it (`br_goto`). Covers mximg / editor / Files at once.
- [x] **T34** — `~/.mxrc` per-user GUI profile, as a shared read-modify-write module (`mxrc.c`: `mxrc_home`/`mxrc_get`/`mxrc_get_int`/`mxrc_set`) so multiple writers coexist (wallpaper + tray keys). Dock **right-click menu** toggles the tray elements (Clock / Date / Network / CPU·RAM / GPU), persisted to `~/.mxrc` (`TrayClock` etc.); read at WM startup. Consolidates the duplicated home/get helpers out of wm.c + mximg.c.
- [x] **T33** — Video-backend indicator on the dock tray (the honest version of "GPU stat" — Makar has no GPU-utilisation metering): new `SYS_VIDEO_NAME` returns the active driver name (`video_active()->name`); the WM shows `GPU <backend>` (e.g. `vbe-1fb`, `svga-ii`), toggleable like the other tray items (`TrayGpu`).

### Image viewer + Doom UX (branch `feat/jpeg-doom-clicklag`)

- [x] **T38** — `mximg` baseline JPEG decode — shared `img_jpg.c` (integer-only: marker parse + Huffman + dequant + fixed-point 8x8 IDCT + chroma upsample + YCbCr→RGB; 4:4:4 / 4:2:2 / 4:2:0 + grayscale + restart markers; progressive rejected). Host-validated against the Go baseline test JPEGs (all subsamplings + grayscale render correctly). Wired into mximg (BMP/GIF/PNG/JPEG).
- [x] **T28** — Fix GUI click lag / double-click. Two causes: (1) `mx_pump` derived press/release from the *final* button state vs the previous pump, so a quick click whose down+up both drained in one pump was dropped — now latch the edges per event; (2) the real one — the WM **suppressed hover motion** (forwarded the pointer only on a button edge/drag), so the focused client's hit-test (`hot`/`mx`) was one event stale and a click landed on the *previously known* position, needing a second click. Now deliver pointer motion to the focused client X11-style (forward on any move over its area; `win_push` coalescing keeps the queue from flooding). Fixes every makx client.
- [x] **T31** — Doom95-style launcher (`mxdoom.elf`): a makx dialog to pick the IWAD (scanned from `/usr/share/games/doom` + `/apps`), an optional PWAD (Browse via the shared file dialog), skill, episode/map, and monster options (No Monsters / Fast / Respawn); "New Game" `execve`s `doom.elf` with the matching `-iwad`/`-file`/`-skill`/`-warp`/`-nomonsters`/`-fast`/`-respawn` args (replaces the launcher in-place so doom keeps the WM-launched pid → still reaped on exit → window closes cleanly). The WM reuses the window on doom's re-HELLO (same pid) and refits it to doom's surface. The Doom desktop shortcut now opens the launcher.

### Gallery + file-manager views (branch `feat/gallery-fileviews`)

- [x] **T42** — `mxfiles` dual view: a **View** toggle switches between a **list view** (name + human size + `YYYY-MM-DD HH:MM` modified date, via `sys_stat` per entry, rendered through `ui_listbox`) and an **icon view** (folder/file glyphs in a grid, arrow-key scrolled) that visually differentiates files from folders. Icon-view cells are widened with a gutter so long labels clip to the padded width instead of butting against the neighbouring cell.
- [x] **T43** — `mximg` **Gallery**: a Gallery/Viewer toggle shows a scrollable thumbnail grid of images found in `~/Pictures` + the bundled `/usr/share/pixmaps` and `/usr/share/backgrounds`; thumbnails decode lazily (aspect-fit, reusing the shared BMP/GIF/PNG/JPEG decoders) and a click opens the full image in the viewer.
- [x] **T44** — Default-app dispatch (file associations): a new `MX_OPEN` makx request hands the WM a file path (over a throwaway shared surface, since it doesn't fit the IPC payload) and the WM opens it in the right app — images → `mximg`, `.htm`/`.html` → `mxweb`, `.elf` makx GUI apps (mx-prefixed, `gui`, `doom`) run directly, other executables in a terminal (`mxterm`), everything else in the editor (`mxedit`). The **WM is the launcher** so the opened window is its child and is reaped normally. Double-clicking/opening a regular file in `mxfiles` routes through it (`mx_open`).
- [x] **T45** — Fix GUI multitasking starvation (kernel IPC fairness): the `ipc_send` slow-path enqueued blocked senders **LIFO** (prepended at the head), so a client that polls the display server every frame (block → served → immediately re-send) kept re-inserting at the head and the server's bounded per-frame drain never reached an older waiter behind it — a newly launched app's one-shot `MX_HELLO` starved (stuck on "starting…") until the busy app exited. Made the sender queue **FIFO** (append at the tail) so a queued message is always served ahead of anything enqueued after it. Fixes "open a second GUI app and it hangs until you close the first" for every makx client; `ipc.c` only, ktest still 885/0.
- [x] **T46** — Doom window respects the GUI window size: the compositor now scales a fixed-size (non-reflow) client — Doom renders a fixed internal frame — to **fill the window preserving aspect ratio, centred, with black letterbox/pillarbox bars**, instead of pinning it 1:1 in the corner when the window grew. Drag-resize + maximize already worked for any window; now they actually enlarge the game. Doom also launches **enlarged** (the largest integer multiple of its native frame that fits the desktop) instead of at its tiny native size — keyed off `MX_F_RAWKEYS` (the game flag) in the re-HELLO refit.
- [x] **T47** — Fullscreen (non-GUI) Doom actually switches to graphics (the third launch context, alongside the desktop launcher icon and a GUI terminal). The console path's `SYS_FB_PRESENT` was dropped by the `vtty_is_focused()` gate (which depends on the app being the focused VT's foreground task — not guaranteed across launch paths / tty slots), so the game ran but its frames silently no-op'd and the text console stayed frozen — in **both** a text-mode boot and after exiting the GUI. The gate now only enforces focus **while the GUI owns scanout** (`vtty_root_gui_active()`); in a plain text console the fullscreen app *is* the display owner and presents regardless of its VT slot (raw-keyboard passthrough blocks VT-switching mid-frame, so it can't bleed onto another VT). Doom keeps normal behaviour: WAD-load chatter prints to the console, then the first game frame takes over the framebuffer (`fb_touched` makes the VT repaint on exit). **Second cause (the real culprit, found via serial markers):** `SYS_FB_PRESENT` then returned −1 because it validates the *entire* framebuffer extent is mapped, but Doom's fullscreen back buffer is an anonymous (demand-paged) mmap and Doom only ever redraws the centred 640×400 region — the letterbox-margin pages stayed unfaulted, so every present failed and nothing appeared (the windowed path is immune: its shared surface is fully mapped). `DG_Init` now `memset`s the whole buffer once to fault every page in (and draw the black border). `DG_Init` also guards the pure-VGA-text case (`fb_info==0`): bail with a message instead of a zero-sized buffer. (A separate non-makx Doom binary was considered but unnecessary — the makx link is inert on the fullscreen path; this was a paging bug, not a dependency one.)
- [x] **T48** — `/dev/null` device + GUI-terminal apps open a window (X11 `$DISPLAY` idiom). (a) Added `/dev/null` to devfs (writes discarded, reads EOF) — generally useful for redirecting unwanted output. (b) New `SYS_MAKX_SERVER` returns the running display-server (`gui.elf`) pid, 0 if no GUI; `mx_connect` falls back to it when there's no `-makx` argv handle, so a makx app launched from a **GUI terminal** (e.g. `doom` in `mxterm`) auto-discovers the server and opens its **own window** while its stdout keeps flowing to the terminal — only a true text console (no GUI) falls through to the fullscreen path. (c) For windows of clients the server didn't fork (no `wait4` relationship), the WM probes liveness with `kill(pid,0)` (now a POSIX existence test) and closes the window when the client exits — no more ghost windows; makes "any process is a makx client" robust.

---

## To do (backlog)

- [ ] **T20** — AHCI (SATA) + a `blkdev` vtable
- [ ] **T21** — USB HID keyboard + mouse (UHCI → xHCI)
- [ ] **T22** — UEFI boot survivability (OVMF)
- [ ] **T23** — Hyper-V synthvid (VMBus) display backend
- [ ] **T24** — `mxweb` — HTTP over lwIP + a minimal HTML renderer
- [ ] **T30** — Windowed framebuffer for console-launched graphical apps (Doom in a makx window)
- [ ] **T35** — Memory management continuation (PMM accounting + reclaim/GC tuning)
- [ ] **T36** — Self-hosted i686-makar toolchain → GHCR (parked)
- [ ] **T37** — Linux-ification: cut the ~101-syscall surface toward UNIX idioms (device files, ioctl, getdents)
