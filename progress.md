# Makar — progress

Tasks are incrementally numbered T1…TN, split into **Done** and **To do**.
Last reordered 2026-06-09 (after PR #204 merged).

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

### PR 3 — image decoders + desktop overhaul (PR #203, merged)

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

### PR 4 — image viewer (JPEG/gallery) + file manager + default-app + doom + multitasking (PR #204, merged)

- [x] **T38** — `mximg` baseline JPEG decode — shared `img_jpg.c` (integer-only: marker parse + Huffman + dequant + fixed-point 8x8 IDCT + chroma upsample + YCbCr→RGB; 4:4:4 / 4:2:2 / 4:2:0 + grayscale + restart markers; progressive rejected). Host-validated against the Go baseline test JPEGs (all subsamplings + grayscale render correctly). Wired into mximg (BMP/GIF/PNG/JPEG).
- [x] **T28** — Fix GUI click lag / double-click. Two causes: (1) `mx_pump` derived press/release from the *final* button state vs the previous pump, so a quick click whose down+up both drained in one pump was dropped — now latch the edges per event; (2) the real one — the WM **suppressed hover motion** (forwarded the pointer only on a button edge/drag), so the focused client's hit-test (`hot`/`mx`) was one event stale and a click landed on the *previously known* position, needing a second click. Now deliver pointer motion to the focused client X11-style (forward on any move over its area; `win_push` coalescing keeps the queue from flooding). Fixes every makx client.
- [x] **T31** — Doom95-style launcher (`mxdoom.elf`): a makx dialog to pick the IWAD (scanned from `/usr/share/games/doom` + `/apps`), an optional PWAD (Browse via the shared file dialog), skill, episode/map, and monster options (No Monsters / Fast / Respawn); "New Game" `execve`s `doom.elf` with the matching `-iwad`/`-file`/`-skill`/`-warp`/`-nomonsters`/`-fast`/`-respawn` args (replaces the launcher in-place so doom keeps the WM-launched pid → still reaped on exit → window closes cleanly). The WM reuses the window on doom's re-HELLO (same pid) and refits it to doom's surface. The Doom desktop shortcut now opens the launcher.
- [x] **T42** — `mxfiles` dual view: a **View** toggle switches between a **list view** (name + human size + `YYYY-MM-DD HH:MM` modified date, via `sys_stat` per entry, rendered through `ui_listbox`) and an **icon view** (folder/file glyphs in a grid, arrow-key scrolled) that visually differentiates files from folders. Icon-view cells are widened with a gutter so long labels clip to the padded width instead of butting against the neighbouring cell.
- [x] **T43** — `mximg` **Gallery**: a Gallery/Viewer toggle shows a scrollable thumbnail grid of images found in `~/Pictures` + the bundled `/usr/share/pixmaps` and `/usr/share/backgrounds`; thumbnails decode lazily (aspect-fit, reusing the shared BMP/GIF/PNG/JPEG decoders) and a click opens the full image in the viewer.
- [x] **T44** — Default-app dispatch (file associations): a new `MX_OPEN` makx request hands the WM a file path (over a throwaway shared surface, since it doesn't fit the IPC payload) and the WM opens it in the right app — images → `mximg`, `.htm`/`.html` → `mxweb`, `.elf` makx GUI apps (mx-prefixed, `gui`, `doom`) run directly, other executables in a terminal (`mxterm`), everything else in the editor (`mxedit`). The **WM is the launcher** so the opened window is its child and is reaped normally. Double-clicking/opening a regular file in `mxfiles` routes through it (`mx_open`).
- [x] **T45** — Fix GUI multitasking starvation (kernel IPC fairness): the `ipc_send` slow-path enqueued blocked senders **LIFO** (prepended at the head), so a client that polls the display server every frame (block → served → immediately re-send) kept re-inserting at the head and the server's bounded per-frame drain never reached an older waiter behind it — a newly launched app's one-shot `MX_HELLO` starved (stuck on "starting…") until the busy app exited. Made the sender queue **FIFO** (append at the tail) so a queued message is always served ahead of anything enqueued after it. Fixes "open a second GUI app and it hangs until you close the first" for every makx client; `ipc.c` only, ktest still 885/0.
- [x] **T46** — Doom window respects the GUI window size: the compositor now scales a fixed-size (non-reflow) client — Doom renders a fixed internal frame — to **fill the window preserving aspect ratio, centred, with black letterbox/pillarbox bars**, instead of pinning it 1:1 in the corner when the window grew. Drag-resize + maximize already worked for any window; now they actually enlarge the game. Doom also launches **enlarged** (the largest integer multiple of its native frame that fits the desktop) instead of at its tiny native size — keyed off `MX_F_RAWKEYS` (the game flag) in the re-HELLO refit.
- [x] **T47** — Fullscreen (non-GUI) Doom actually switches to graphics (the third launch context, alongside the desktop launcher icon and a GUI terminal). The console path's `SYS_FB_PRESENT` was dropped by the `vtty_is_focused()` gate (which depends on the app being the focused VT's foreground task — not guaranteed across launch paths / tty slots), so the game ran but its frames silently no-op'd and the text console stayed frozen — in **both** a text-mode boot and after exiting the GUI. The gate now only enforces focus **while the GUI owns scanout** (`vtty_root_gui_active()`); in a plain text console the fullscreen app *is* the display owner and presents regardless of its VT slot (raw-keyboard passthrough blocks VT-switching mid-frame, so it can't bleed onto another VT). Doom keeps normal behaviour: WAD-load chatter prints to the console, then the first game frame takes over the framebuffer (`fb_touched` makes the VT repaint on exit). **Second cause (the real culprit, found via serial markers):** `SYS_FB_PRESENT` then returned −1 because it validates the *entire* framebuffer extent is mapped, but Doom's fullscreen back buffer is an anonymous (demand-paged) mmap and Doom only ever redraws the centred 640×400 region — the letterbox-margin pages stayed unfaulted, so every present failed and nothing appeared (the windowed path is immune: its shared surface is fully mapped). `DG_Init` now `memset`s the whole buffer once to fault every page in (and draw the black border). `DG_Init` also guards the pure-VGA-text case (`fb_info==0`): bail with a message instead of a zero-sized buffer. (A separate non-makx Doom binary was considered but unnecessary — the makx link is inert on the fullscreen path; this was a paging bug, not a dependency one.)
- [x] **T30** — Console-launched graphical apps appear in a makx window — achieved via the `$DISPLAY`-style discovery (T48) rather than a separate SDL-style backend: a graphical app launched from a GUI terminal (e.g. `doom` in `mxterm`) auto-connects to the display server and opens its own window, while a true non-GUI text console runs it fullscreen (T47). No generic shim needed — `doom` is a native makx client.
- [x] **T48** — `/dev/null` device + GUI-terminal apps open a window (X11 `$DISPLAY` idiom). (a) Added `/dev/null` to devfs (writes discarded, reads EOF) — generally useful for redirecting unwanted output. (b) New `SYS_MAKX_SERVER` returns the running display-server (`gui.elf`) pid, 0 if no GUI; `mx_connect` falls back to it when there's no `-makx` argv handle, so a makx app launched from a **GUI terminal** (e.g. `doom` in `mxterm`) auto-discovers the server and opens its **own window** while its stdout keeps flowing to the terminal — only a true text console (no GUI) falls through to the fullscreen path. (c) For windows of clients the server didn't fork (no `wait4` relationship), the WM probes liveness with `kill(pid,0)` (now a POSIX existence test) and closes the window when the client exits — no more ghost windows; makes "any process is a makx client" robust.

---

## To do (backlog) — ordered by priority

### Up next — networking / web
- [x] **T24a** — `mxweb` **Phase 1 (HTTP)**: a Dillo-like makx browser — from-scratch HTML tokeniser + block/inline renderer (headings, paragraphs, lists, bold, links, `<hr>`, word-wrap, entities), inline images (PNG/JPEG/BMP via the shared decoders), scroll + scrollbar, clickable links, a **Back / Fwd / Home / Go** toolbar with history, and direct local-file `.html` open (the `.html → /apps/mxweb.elf` default-app hook + `$DISPLAY`-style discovery from T44/T48). Globe desktop icon + `13-web.desktop`; offline `about:start` home page. CSS/JS stripped; `https://` reports cleanly (no TLS yet).
- [x] **T24b** — `mxweb` **Phase 2 (minimal CSS)**: a small CSS engine — `<style>` rules (`tag` / `.class` / `#id`, specificity-lite) + inline `style=` → `color` / `background[-color]` / `font-weight`, applied through a nesting style stack with per-line background fills. Plus **scaled headings** (integer-upscaled 8×8 glyphs: h1 3×, h2/h3 2×, via new `gfx_char_scaled`/`gfx_str_scaled`) and **PNG alpha composited over white** in the shared `img_png` (transparent logos no longer render as a black box). Also `font-size`→glyph scale, `rgb()` colours, and a **centred max-width card** (`width` + `margin:auto`) painted on the body background. JS still ignored; floats/absolute-positioning not modelled (block flow only).
- [x] **T24d** — `mxweb` **HTML forms (web search)**: `<form>` + text/search/password/hidden `<input>` render as editable in-page boxes (click to focus, type, caret, `*` masking); a Submit button or **Enter** urlencodes the `name=value` pairs onto the form `action` and navigates — so **search works** (frogfind over HTTP now; Wikipedia once TLS lands). Field focus is mutually exclusive with the URL bar; hidden fields ride along in the query.
- [x] **T24c** — `mxweb` **Phase 3 (HTTPS)** — done via the ring-3 TLS layer (T51.2); https://en.wikipedia.org renders.
- [x] **T24e** — `mxweb` **configurable homepage** (`~/.mxwebrc`): reads `Homepage=<url>` at startup (hand-editable), the **Home** button and initial load use it, and a **`+H`** toolbar button pins the current page as the homepage (written via the shared `mxrc_*_file` rc helpers; default `about:start`).
- [ ] **T50** — `linx` — a **lynx/links-style text-mode** browser (freestanding ELF) reusing mxweb's HTML engine, reflowed to the terminal with ANSI + numbered links and key navigation. Shares the fetch layer, so HTTPS arrives with T51. (A literal GNU lynx/links port needs ncurses + a hosted libc/OpenSSL — deferred to the self-hosted toolchain.)
- [ ] **T51** — **Shared TLS layer** for HTTPS (mxweb/linx) + SSH (T49): TLS 1.2 (ECDHE + AES-GCM/ChaCha20, SNI, X.509). **Decided: vendor BearSSL** (`git.bearssl.org`, reachable). Start with in-kernel `wgets` (both browsers already call `sys_wget` → fastest path); add userspace TCP-socket syscalls + gzip/chunked HTTP/1.1, reused by SSH. Note: CloudFlare JS-challenge pages render but can't be solved (no JS) — handle gracefully.
  **In-kernel BearSSL was tried and abandoned** (vendored `vendor/bearssl` + a kernel static archive; the client compiles, links, and starts a real handshake to Wikipedia): but the handshake **overflows the 8 KB ring-0 task stack** (`TASK_STACK_SIZE`, cert-chain parse + RSA/EC bignum) → kernel page-fault. Crypto-in-ring-0 is also wrong per our golden rule (kernel = sockets, userspace = TLS) and any fault wedges the whole machine.
  **Decided — TLS in userspace:** add kernel **TCP-socket syscalls** (socket/connect/send/recv/close + a DNS-resolve syscall, over the raw-lwIP poll loop), build **BearSSL into the userspace tree** (link into mxweb / a shared `libtls`), run the handshake in ring 3 (32 KB user stack; a fault just kills the tab), then strip the in-kernel BearSSL. Already-written logic that carries over: HTTP **redirect-following** (`wget_fetch` 3xx→Location, in the kernel now), the **RDRAND CPUID gate** (qemu-system-i386's `qemu32` has no RDRAND → #UD froze the kernel), and the I/O timeouts. *In-kernel TLS is currently disabled* — `https://` returns a clean "moving to userspace" message, no crash.
  - [x] **T51.1 — kernel TCP-socket syscalls (the foundation) — landed.** `SYS_SOCKET`/`SYS_CONNECT`/`SYS_NET_RESOLVE` (280–282); `read()`/`write()`/`close()` route through a new `FD_KIND_SOCKET`. `ksock` core in `net/net_lwip.c` (fixed pool over the lwIP raw API, co-located with the net big-lock, lock-and-pump like `net_lwip_resolve`, bounded timeouts). `sockaddr_in` in `makar_abi.h`; BSD wrappers + `sys_tcp_connect()` in userspace. Builds clean, ktest 885/0. The OSI/ring line now sits at the socket (kernel L1–L4, userspace L5–L7) — see `krnlsepr.md`.
  - [x] **T51.2 — userspace HTTP + TLS, VERIFIED: mxweb renders https://en.wikipedia.org.** `web.c` (HTTP/1.1 over kernel sockets: http:// plain, https:// via BearSSL, 3xx redirects, chunked) + `tls.c` (BearSSL client over a socket fd; handshake on the 32 KB user stack, heap-allocated contexts). BearSSL compiles into a userspace static `libbearssl.a` (objects under `bssl-obj/`; `-nostdinc` dropped for those so `<x86intrin.h>` resolves, like the kernel build). **mxweb + wget.elf fetch entirely in ring 3** — no more `sys_wget`; `mxweb.elf` 163K→1.1M. Confirmed: full Main Page fetched (241 KB, `tls_err=0`) + rendered. **Handshake fix:** feed the RTC wall clock to `br_x509_minimal_set_time` (Unix epoch = days 719528) — without a clock BearSSL stalled at `BR_ERR_X509_TIME_UNKNOWN`→`UNEXPECTED`; `na_end_chain` also swallows NOT_TRUSTED/TIME_UNKNOWN/EXPIRED (encrypt-only, not authenticated). Per-fetch `[web]`/`[ksock]` serial diagnostics retained.
  - [ ] **T51.3 — strip the in-kernel HTTP/TLS** now that ring 3 owns it: remove `SYS_WGET` + `cmd_wget` + `wget_fetch`/`wget_tls` from `shell_cmd_net.c`, and `libbearssl.a` from the kernel image (make.config / `Makefile`). End state: kernel = TCP/IP + sockets only (empties the http + TLS boxes in `krnlsepr.md`).
- [ ] **T49** — SSH over the NIC, on the T51 crypto layer. **Target: a Dropbear port** (lightweight, self-contained crypto/SSH); needs the userspace TCP sockets + a pty path.

### Self-hosted musl libc + dynamic linking (EPIC, LANDED — branch `feat/dynamic-libc`)
Ship **musl as a shared `libc.so`** and run **dynamically-linked** programs, using
musl's own dynamic linker (`ld-musl-i386.so.1`).  Reuses the existing host musl
cross-toolchain in `toolchain/` (already produces `libc.so` + the interpreter +
PIE startfiles).  Scope: capability + demo; the ~50 in-tree apps stay static.
Chosen over newlib / an own-libc / an own dynamic linker.  Plan:
`~/.claude/plans/melodic-honking-river.md`.
- [x] **Phase 0 — static musl runs in-OS.** A real static-musl `hello`
  (`toolchain/test/hello.c`, staged via `toolchain/build-musl-demos.sh` as
  `/apps/muslhello.elf`, linked at `0x40000000`) executes end-to-end: musl
  startup (auxv walk, `set_thread_area` TLS, `futex` locks), `argc` plumbing,
  `printf`/`fprintf`, buffered-stdout flush on `exit()`, clean exit 0.  Found +
  fixed the blocker: **`SYS_WRITEV` (146) was unimplemented** — musl's stdio
  writes through `writev`, so all libc output was silently dropped.  Implemented
  it sharing one `syscall_fd_write` dispatch with `SYS_WRITE`.  Smoke-gated
  (`shell-smoke.sh: musl-static`); full gate stays 885/0.
- [x] **Phase 1 — dynamic build path + musl staged.** `build-musl-demos.sh`
  links `muslhellodyn.elf` as a PIE (`-pie -fPIE` → `ET_DYN`,
  `PT_INTERP=/lib/ld-musl-i386.so.1`) and stages `libc.so` + `ld-musl-i386.so.1`
  into `/lib`.  Wired into `run.sh`'s `_build_iso` (dev-only; skipped in CI / when
  the host cross-toolchain is absent, so the suite's musl tests then SKIP).
- [x] **Phase 2 — file-backed `mmap` + `MAP_FIXED` + `mprotect`.** `SYS_MMAP2`
  (192) now honours a real `fd`: it eager-reads the file region into private
  frames (zero-filling the bss tail) and maps `MAP_FIXED` at the caller's exact
  address; anon non-fixed maps keep the demand-paged reserve.  New `SYS_MPROTECT`
  (125) rewrites PTE R/W/USER bits over a range via `vmm_protect_page` (RELRO).
- [x] **Phase 3 — dynamic ELF loader.** `elf_exec` runs `ET_DYN` PIEs: load base
  `0x50000000`, interp (`ld-musl`) at `0x70000000`, a full System-V i386 auxv
  (`AT_PHDR`/`PHENT`/`PHNUM`/`BASE`/`ENTRY`/`EXECFN`/`RANDOM`/`PAGESZ`), enter at
  the interpreter.  `AT_PHDR` resolves via the `PT_LOAD` that maps `e_phoff`.  The
  `ET_EXEC` path is byte-for-byte unchanged (a vestigial `PT_INTERP`, e.g. TCC's
  `/lib/ld-linux.so.2`, is ignored — only `ET_DYN` consults it).
- [x] **Phase 4/5 — verified + errno + docs.** `muslhellodyn.elf` runs
  end-to-end: kernel → `ld-musl` → `mmap`s `libc.so` into the `0x90000000`
  window → relocates → RELRO `mprotect` → `main` prints via `writev`, exit 0
  (`shell-smoke.sh: musl-dynamic`).  `SYS_OPEN` now returns `-ENOENT` (not `-1`)
  on a missing file so `ld.so` can walk its search path.  Full gate 885/0.
- [x] **Phase 6 — page-cache-backed *shared* file mmap (the RAM win).** Without
  this, every dynamic process got a *private* ~800 KiB copy of `libc.so` —
  dynamic linking would *cost* RAM vs the static shim.  Now the page cache backs
  each page with a refcounted PMM frame (`pagecache_acquire`), and a read-only
  file `mmap` maps that shared frame straight in: libc.so's ~700 KiB of
  text/rodata is **one copy in RAM** for every process (writable/data stays
  private; the Linux page-cache model).  Also fixed two latent teardown bugs:
  `munmap` never freed frames (`vmm_unmap_and_free`), and `vmm_free_pd`/
  `vmm_clone_pd_cow` skipped kernel-shared page tables with an *exact* PDE
  compare that the CPU's async Accessed-bit updates defeated — so teardown could
  mis-free the framebuffer's kernel PT (the `0x2EA` refcount-underflow warning);
  now compares the PT frame address only.  Proven by the `pagecache_share`
  ktest (a 2nd mapper of a page allocates **zero** new frames).  Gate 894/0.
  This **unblocks** the userspace static→dynamic migration (was gated on exactly
  this so it wouldn't regress memory on a 32 MiB kernel).

### Desktop UX (T52) — framework landed, wiring in progress
- [x] **T52.1 — menu/window framework (PR #205).** `gui_ui` gained **`ui_menubar`** (File/Edit/View/Help bar + dropdowns), **`ui_context_menu`** (right-click popup), **`ui_about`** (modal credits card) — Windows-style: greyed disabled items, separators, right-aligned accelerators, dropdown width sized to the longest label. **`ui_btn_w()`** centralises button-label padding (mxweb toolbar = first adopter). **Double-click a title bar → maximise/restore** (`wm.c`). `LICENCES/` collects every external licence (BearSSL/lwIP/doomgeneric/FreeDoom/TinyCC/Limine/musl) with per-file coverage notes, feeding the About dialogs.
- [x] **T52.2 — wire the menus into apps.** Kernel **clipboard syscalls** (`SYS_CLIP_SET/GET` + buffer) for cross-app Cut/Copy/Paste; per-app File/Edit/View/Help bars + right-click context menu (Cut/Copy/Paste/Undo/Redo, enable/disable by context) + **Ctrl-A/C/X/V/Z/Y**; Help→About with per-app credits. **Undo/redo** = two bounded LIFO stacks.
  - [x] **makx right mouse button** plumbed end-to-end: `wm.c` forwards bit1 in `MXEV_MOUSE`, `mx_pump` exposes `c.rpressed/rdown/rreleased` — the prerequisite for any right-click menu.
  - [x] **`ui_vscroll`** reusable scrollbar widget (track + proportional thumb, click/drag) added to `gui_ui`.
  - [x] **`ui_textbox` clipboard shortcuts** — every textbox now takes **Ctrl-A** (select all, with highlight), **Ctrl-C/X** (copy/cut to the system clipboard; suppressed on password fields) and **Ctrl-V** (paste, replacing a selection). Required **removing the vestigial `Ctrl-A` pane-switch prefix** from the keyboard driver (the makmux/tmux-style `kb_pane[]` machinery was never wired up and was swallowing `Ctrl-A`); it now folds to `0x01` and reaches the focused app like any other control key.
  - [x] **mxedit** (reference): Edit menu + Ctrl-shortcuts + drag-select + Undo/Redo, **right-click context menu** (Cut/Copy/Paste/Select All/Undo/Redo, greyed by context) and a **scrollbar**.
  - [x] **mxfiles**: File/**Edit**/View/Help bar + About + right-click menu + **scrollbar** (list & icon). **Windows-Explorer file operations**: multi-select with **Ctrl-A**, **Ctrl-C/X/V** to copy / cut / paste *real files* into the current folder (collision-safe — pastes never overwrite, they make a "name copy"), **Ctrl-Z/Y** undo/redo the last paste. No delete key, so a move is always reversible. Folder copy (recursive) is the one gap; folder *move* works.
  - [x] **mxweb / mximg**: menu bar + Help→About with per-app credits (mxweb → BearSSL + lwIP; mximg → Arawn Davies).
  - [x] **mxterm**: Edit/Help menu bar + right-click menu (Copy/Paste/Select All), **drag-to-select** cells → clipboard, and a **scrollback buffer** (`vt100` keeps the last `VT_SCROLLBACK=500` evicted lines) with a draggable **scrollbar**; typing snaps to the live tail. Ctrl-C still reaches the shell as SIGINT (copy/paste are menu/RCCM/drag, terminal-correct).
  - [x] **mxweb**: File/**Edit**/View/Help bar; **drag-to-highlight page text** (the renderer records each drawn word as a doc-space run; a drag maps to a character range, highlighted live) → **Copy** via Ctrl-C / Edit menu / right-click; Select All. Selection resets on navigation.
  - [x] **System-wide chrome**: `gui_ui` gained **`ui_appbar`** (a standard File→Exit + Help→About&lt;app&gt; bar with per-app credits) and **`ui_gate`** (one-call content-input gating while a menu is open), so every window gets the same menu bar in ~3 lines. Rolled out to **mxclock, mxcalc, mxtasks** (right-click row → Kill/Refresh), **mxnet, mxdisk, mxdisplay** — joining mxedit/mxfiles/mxweb/mximg/mxterm (**11 apps**). The About dialog (mxabout), the install wizard (mxinstall) and full-screen DOOM keep no bar, per the Windows convention.

### Userspace tooling / terminals
- [ ] **T54 — `tar`** (userspace ELF): USTAR **c / x / t / v / f**, plus **gzip `z`** (decompress via the shared `inflate.h`; create via stored-DEFLATE + CRC32 — real DEFLATE compression is a follow-up). **`xz` not planned** (LZMA too heavy for the hobby target). Started: `inflate.h` extracted (header-only, shared with `img_png`).
- [ ] **T55 — terminal-app mxterm-compliance audit.** Every TUI app must work both in a text VT (kernel line discipline) AND under mxterm (raw pipe: char-by-char, no echo, Enter already mapped to `\n`). `basic` fixed via the `bgetline` pattern (accumulate to newline; self-echo + Backspace only when the input is raw). Audit + fix the rest (cfdisk/fdisk/maktop/…) the same way.
- [ ] **T56 — mxterm ASCII→UTF-8, full ANSI/VT100.** It's already a working ANSI/VT100 emulator (hosts the TUIs); add UTF-8 decode. Longer goal: enough fidelity to host a serial-terminal app (minicom-style) — Makar as a dumb terminal over COM/USB-serial.

#### Polish landed this session (PR #205, branch `feat/mxweb`)
- [x] `mxweb`: HTTPS verified (Wikipedia renders) + `~/.mxwebrc` homepage + auto-padded toolbar; HTTP+TLS fully in ring 3.
- [x] `vix` opens a blank buffer with no filename (GUI launch); `.bas` files open in BASIC (loaded, "type RUN to run") from the file browser.
- [x] `basic` line input works under mxterm (was syntax-erroring every keystroke).
- [x] Terminal-app desktop shortcuts (vix/maktop/basic/cfdisk) hosted in mxterm, terminal icon.
- [x] ISO `/src` stripped of build artefacts (1594→657 files) — also fixes the slow HDD install (per-file write cost of ~900 tiny objects).
- [x] Docs: `krnlsepr.md` (kernel/userspace separation audit), `terminals.md` (tty/pty explainer).

### Hardware / platform
- [ ] **T20** — AHCI (SATA) + a `blkdev` vtable
- [ ] **T21** — USB HID keyboard + mouse (UHCI → xHCI)
- [ ] **T22** — UEFI boot survivability (OVMF)
- [ ] **T23** — Hyper-V synthvid (VMBus) display backend

### Kernel / toolchain
- [ ] **T35** — Memory management continuation (PMM accounting + reclaim/GC tuning)
- [ ] **T37** — Linux-ification: cut the ~101-syscall surface toward UNIX idioms (device files, ioctl, getdents)
- [ ] **T36** — Self-hosted i686-makar toolchain → GHCR (parked)
