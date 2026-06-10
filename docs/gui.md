# Makar GUI / windowing

The Makar desktop is an **X11-style display server + clients** ("makx").
`gui.elf` (`src/userspace/wm.c`) is the **display server**: it owns the
framebuffer, the keyboard and the mouse, draws the desktop chrome (window
borders, dock, menu bar, cursor) and composites windows. Each application is a
separate **client** process (`mxterm.elf`, `mxfiles.elf`, `mxedit.elf`,
`mxtasks.elf`, `doom.elf`) that talks to the server over the kernel's
synchronous IPC for control and a **shared pixel surface** for pixels — control
over the message channel, bulk pixels over shared memory, the same split X11
draws between its protocol socket and MIT-SHM. The protocol + client library is
`src/userspace/makx.{h,c}`.

## Design constraints

Two kernel facts shape the whole design:

1. **The WM is the sole compositor.** `SYS_FB_PRESENT` / `SYS_DRAW_LINE` are
   focus-gated (`vtty_is_focused`, `proc/vtty.c`). The WM is the single focused
   root-GUI task, so only it may blit the framebuffer. Every window — including
   graphical children like DOOM — renders into an off-screen buffer that the WM
   reads and composites. Nothing else touches scanout.

2. **No userspace shared memory except surfaces.** `fork` is copy-on-write and
   `SYS_MMAP2` is private-anon, so a forked child cannot hand pixels back to the
   WM through ordinary memory. The kernel **shared surface** primitive
   (below) is the bridge.

Because the WM owns input and composites everything, **keyboard/mouse focus is
managed entirely inside the WM** — the kernel focus model is untouched. The WM
holds kernel focus; it routes input to the active window. Clicking a window
makes it active (keyboard target) and raises it; the window under the cursor
receives mouse motion.

## Display drivers (kernel)

`kernel/video.h` + `display/video.c` -- a small driver vtable so the kernel binds
an accelerated backend when its hardware is present and falls back to the dumb
linear framebuffer otherwise (the same seam the block-device and filesystem
layers use). `video_init()` runs after the PCI scan; `SYS_FB_PRESENT[_RECT]` and
the WM cursor route through `video_active()`.

- **`video_vbe`** (default) -- present is the per-line CPU memcpy into the LFB;
  software cursor (no caps). Covers QEMU-std, Bochs/DISPI, VirtualBox VGA, bare
  metal, and the adopted GOP/bootloader LFB (Hyper-V Gen2, etc.).
- **`video_svga2`** -- VMware/VirtualBox SVGA II (PCI `15ad:0405`). Mode-sets via
  `SVGA_REG_*`, adopts the device FB, and on present CPU-copies the dirty rect
  then issues a FIFO `SVGA_CMD_UPDATE` so the host scans it out (a plain LFB
  write may never reach the screen on these adapters). Advertises a **hardware
  cursor** (`VID_CAP_HW_CURSOR`). Testable via `MAKAR_VGA="-device vmware-svga"
  ./run.sh guitest`.

### Display settings (`mxdisplay`)

`mxdisplay.elf` changes the screen resolution at runtime. Apply calls
`SYS_SETMODE`; the kernel repoints the framebuffer and the WM **reflows in
place** (`wm_reinit_display`: realloc the back buffer, clamp/re-fit windows,
recomposite) -- no process restart or re-login. After applying it shows a
**"Keep this resolution?"** prompt that auto-reverts to the previous mode after
15 s if not confirmed (the safe-monitor pattern), so an unsupported/garbled mode
can't lock the user out. Runtime mode-set rides the Bochs/DISPI path
(`admin_setmode`), available on QEMU-std / Bochs / VirtualBox; on a pure SVGA II
adapter without DISPI the apply fails gracefully ("mode not supported") -- wiring
runtime mode-set through the SVGA II driver is a follow-up.

When the active driver advertises `VID_CAP_HW_CURSOR`, the WM uploads its arrow
sprite once (`sys_hwcursor_define`) and just moves the overlay
(`sys_hwcursor_move`), skipping the software save-under compositing entirely --
a pure mouse move then costs **no** framebuffer traffic. Without it the WM keeps
the software cursor (capture/restore + small `present_rect` boxes). Syscalls:
`SYS_VIDEO_CAPS`, `SYS_HWCURSOR_{DEFINE,MOVE,SHOW}` (see `docs/syscalls.md`).

While a launched client has forked but not yet sent its surface
(`wm_busy()` -- any window with `client>=0 && sid<0`), the WM shows a **busy
hourglass** cursor instead of the arrow: it re-uploads the HW-cursor sprite on
the accelerated path, or swaps the software bitmap otherwise.

### Settings (`mxsettings`)

`mxsettings.elf` is the **centralised Settings app**, laid out like macOS System
Settings: a left **sidebar** of categories (`ui_listbox`) + a right **content
pane** of grouped rows. It **reimplements each panel inline** rather than
shelling out to the single-purpose apps, driving the underlying mechanisms
directly (the `~/.mxrc` prefs, `SYS_SETMODE`, the net syscalls, the new
clock/network syscalls). The standalone `mxdisplay`/`mxnet` and the dock's
right-click tray toggle still exist; Settings is the one place that gathers them.
Panels:

- **Appearance** — wallpaper path field → writes `~/.mxrc` `Wallpaper=`; the WM
  already polls that key each frame (`apply_wallpaper()`), so the change is live
  with no image decoder in `mxsettings`. (Colour schemes are a stub for now.)
- **Display** — ports `mxdisplay`'s logic inline: the `MODES[]` list,
  `sys_fb_info()` for the current mode, `sys_setmode()` to apply, and the same
  15 s **confirm / auto-revert** countdown.
- **Status Bar** — `ui_toggle` switches for the five `Tray*` prefs
  (`TrayClock/TrayDate/TrayNet/TrayStats/TrayGpu`). On change it writes the
  `~/.mxrc` int **and** sends `MX_RELOAD_PREFS` so the dock re-reads them and
  redraws **immediately** (without it, tray changes would only apply next login).
- **Network** — live `sys_net_info` status; DHCP **Renew/Release/Flush** via
  `sys_net_ctl`; a "Use a static address" toggle revealing IP/Netmask/Gateway/DNS
  fields → `SYS_NET_CONFIG` (static), or "Switch to DHCP".
- **Date & Time** — a live UTC clock (`sys_gettimeofday` + `gmtime_r`) + editable
  Y/M/D H:M:S fields → `SYS_SETTIME` (writes the CMOS RTC; the tray clock follows
  on the next read).
- **Autostart** — a toggle per GUI app; on present its path is in the `~/.mxrc`
  `Autostart=` comma-separated list.

**`MX_RELOAD_PREFS` (makx msg 8):** a no-argument client→server message asking
the WM to re-run `load_tray_prefs()` and repaint. `mx_reload_prefs(c)` sends it;
the `wm.c` poll loop handles it (re-read prefs → `damage_full()`).

**Autostart at login:** `wm.c`'s `load_autostart()` runs once at desktop startup
— it reads the `~/.mxrc` `Autostart=` list and `launch_cmd`s each entry. This is
Makar's prefs-model take on XDG `~/.config/autostart/*.desktop` (a noted future
refinement). The Settings → Autostart panel is just the editor for that list.

A first-class desktop launcher ships too: `data/icons/settings.bmp` (a gear, the
same 32×26 tile format as the other icons) + `data/shortcuts/18-settings.desktop`
+ a fallback entry in `wm.c`'s built-in `icon_defs[]`.

## Shared pixel surfaces (kernel)

`kernel/surface.h` + `arch/i386/proc/surface.c`. A surface is a kernel-owned run
of physical frames mappable into several tasks at once (same frames, distinct
virtual addresses). Syscalls: `SYS_SURFACE_CREATE/MAP/INFO/DESTROY` (see
`docs/syscalls.md`).

Lifetime is reference-counted: alive while the creator ref is held **or** any
task still maps it. `surface_release_task()` runs from `task_terminate()` and
unmaps a dying task's surface pages from its page directory *before*
`vmm_free_pd()` walks it — without this the shared frames, still mapped by
another holder, would be double-freed. Covered by the `surface` ktest suite.

Usage pattern (driven by the makx protocol, below): the **server** owns each
surface (it is the creator, so lifetime is robust against a client crash); the
client maps it and renders into it:

```
client: HELLO(w,h) ----------------------------> server: id=surface_create(w,h)
        base=surface_map(id) <---(win,id)------          base=surface_map(id)
        render frames into base; never fb_present
        PRESENT(win) ---------------------------> server: blit base into the
                                                           window rect; reply
                                                           one queued input event
        (on client exit) server reaps it and surface_destroy(id)
```

## Widget framework (userspace)

`src/userspace/gui_gfx.{c,h}` + `gui_ui.{c,h}` (flat in `src/userspace/`, since
the userspace build is single-directory `-nostdinc -I.`). Linked into `gui.elf`
and reusable by future surface-rendering apps.

- **gui_gfx** — a `gfx_surface` (XRGB8888 pixel buffer + w/h) and clipped
  primitives: `gfx_px/fill/outline/round/char/str/str_clip`, `gfx_blit` (region
  copy, for compositing window contents) and `gfx_blit_scaled` (nearest-
  neighbour, for fitting a fixed app surface like DOOM's 640x400 into a window).
- **img_bmp** / **img_png** / **img_ico** — shared image decoders: BMP
  (`bmp_decode`/`bmp_load`), PNG (`png_decode`/`png_load` — a from-scratch
  RFC1951 inflate + all five scanline filters, bit depths 1-16, colour types
  0/2/3/4/6, non-interlaced) and Windows ICO (`ico_decode`/`ico_load` — a
  directory of BMP-DIB or PNG entries, best size picked). Used by `mximg` and by
  the window manager for desktop icons.

**Desktop icons** are XFCE-style **`.desktop` shortcuts** (`Name`/`Icon`/`Exec`
plus Makar `X-Makar-*` extensions for the launch window size, an extra argv, the
tint and the grid position). The WM scans the system-wide
`/usr/share/shortcuts/*.desktop` plus the user overlay `~/.shortcuts` (which
overrides by filename), building the icon set in `load_desktop_entries`; if both
are empty it falls back to a built-in default list so the desktop is never empty.
Artwork named by `Icon=` is resolved under `/usr/share/icons/makar/` with
`.ico` -> `.png` -> `.bmp` probing (`load_one_icon`), blitted in `draw_icons`;
a missing asset falls back to the procedural `icon_glyph()`. Icons are
**draggable** and **selectable**: a click selects (highlight) + a no-move click
launches, while a drag drops the icon and persists `X-Makar-IconX/Y` back into
the source `.desktop` (best-effort -- a silent no-op on a read-only live ISO).
The `.ico` tiles are generated from the BMP tiles by `tools/bmp2ico.py`
(committed).

**Desktop wallpaper** is opt-in via the image viewer, set the way an X11 desktop
does it -- a config file for persistence plus a shared root pixmap for the live
update. `mximg`'s "Set Wallpaper" button (1) writes `Wallpaper=<path>` into the
per-user `~/.mxrc`, and (2) copies the decoded image into a **shared surface**
and hands the WM its id over `MX_WALLPAPER` (`sid`/`w`/`h`). The WM maps that
surface and blits it stretched (`gfx_blit_scaled`) behind the icons -- applied
instantly, with no cross-process file read (the page cache made re-reading a
just-written `.mxrc` unreliable). At boot the WM loads the persisted path from
`~/.mxrc` instead (`load_image_any`: `.png`/`.ico`/`.bmp`, wallpaper-sized bound
via `bmp_load_max`); a ~1 s poll covers external edits, but a live shared-surface
wallpaper takes precedence. Unset/missing falls back to the flat `COL_DESK`
fill. Example backgrounds ship under `/usr/share/backgrounds`.

The wallpaper path and the **dock tray toggles** live in the per-user `~/.mxrc`
config, accessed through the shared `mxrc.c` module (`mxrc_get`/`mxrc_set`,
read-modify-write so multiple writers -- the WM's tray keys and mximg's
`Wallpaper=` -- coexist). **Right-clicking the dock** opens a small menu to show
or hide each tray element (Clock / Date / Network / CPU·RAM / GPU); the choice
persists to `~/.mxrc` (`TrayClock`/`TrayDate`/`TrayNet`/`TrayStats`/`TrayGpu`)
and is read at WM startup. The **GPU** item shows the active video backend
(`SYS_VIDEO_NAME` -> `video_active()->name`, e.g. `vbe-1fb` / `svga-ii`) rather
than a fabricated utilisation figure -- Makar has no GPU-usage metering. DOOM
IWADs likewise live on a normal path,
`/usr/share/games/doom/` (searched first by `doomgeneric_makar.c`); the installer
copies both with the rest of `/usr`.
- **gui_ui** — an immediate-mode toolkit. Each frame the caller snapshots input
  with `ui_begin` then calls widgets in a fixed order; widget identity is the
  call order. Widgets: `ui_button`, `ui_slider`, `ui_textbox`, `ui_label`,
  `ui_listbox`, `ui_vscroll` (track + proportional thumb, click/drag), the
  standard form controls `ui_toggle` (switch), **`ui_spinner`** (numeric field +
  up/down steppers, clamped), **`ui_radio`**, **`ui_checkbox`**, **`ui_progress`**
  and **`ui_separator`**, and the
  Windows-style menu set `ui_menubar` / `ui_context_menu` / `ui_about` (greyed
  disabled items, separators, accelerators, popup width sized to the longest
  label; they draw **last** so callers gate their own content while a menu is
  open). `ui_textbox` has a **caret model** — Left/Right move it, Home/End jump,
  Delete forward-deletes, a click places it, and the view scrolls to keep it
  visible — plus the clipboard shortcuts on every field: **Ctrl-A** select-all
  (highlighted), **Ctrl-C/X** copy/cut (suppressed on password fields), **Ctrl-V**
  paste — over the kernel clipboard (`SYS_CLIP_*`).
  Transient interaction (pressed/dragged widget, keyboard focus, textbox
  selection) lives in `ui_ctx`; all content is caller-owned. The window manager
  hit-tests windows first and only feeds the focused window a "live" ctx (others
  get a ctx with no buttons/keys so they still draw but don't react) — this is
  how the per-window focus model reaches individual widgets. The pointer ctx
  carries both buttons: `ui_ctx`/`mx_conn` expose left **and** right
  (`rpressed`/`rdown`/`rreleased`) so apps can raise context menus.  Two
  convenience helpers give every window the same chrome cheaply: **`ui_appbar`**
  draws a standard *File → Exit* + *Help → About &lt;app&gt;* bar (plus optional
  Edit/View menus) and the About modal in one call, and **`ui_gate`** /
  `ui_gate_begin` / `ui_gate_end` blank the content ctx's buttons/key while a
  menu or modal is open so widgets drawn under it don't react.  Used by the
  simple apps (clock/calc/tasks/net/disk/display) to carry a menu bar in ~3
  lines; the richer apps add their own Edit/View menus on top.
- **vt100** (`vt100.{c,h}`) — a standalone ANSI/VT100+ terminal emulator core:
  `vt_init`/`vt_resize`/`vt_putc` drive a colour `vt_cell` grid (cursor, scroll
  region, SGR colours, ED/EL, IL/DL/ICH/DCH/ECH, alt-screen, UTF-8 → one cell).
  Used by `mxterm` (and reusable by a future serial console); freestanding (no
  libc). The front-end renders the grid via a 16-colour palette → `gfx_char`.
  A full-screen scroll pushes the evicted top line into a **scrollback ring**
  (`sb[VT_SCROLLBACK]`, alt-screen excluded) that `mxterm` reads for its
  scrollbar. `mxterm` adds an Edit/Help **menu bar**, a right-click **Copy /
  Paste / Select All** menu, and **drag-to-select** over the cells (mapped to
  logical scrollback+screen lines) that copies to the system clipboard; typing
  snaps the view back to the live tail.

### Text-mode (TUI) apps in the GUI terminal — cell-API → ANSI bridge

The existing TUI apps (`maktop`, `vix`, `cfdisk`, the `install` flow, …) don't
write bytes; they call the kernel **cell API** (`SYS_PUTCH_AT`, `SYS_SET_CURSOR`,
`SYS_TTY_CLEAR`, `SYS_TERM_SIZE`) which targets a per-task VT backing grid. A
process forked by `mxterm` has **no live VT slot** and its stdout is a pipe, so
those calls would have nowhere to land.

The kernel bridges them: when a cell-API call comes from a task with
`vtty_buf_current() == NULL` **and** fd 1 is a pipe, it is translated to the
equivalent **ANSI escape** written to fd 1 — `SYS_PUTCH_AT` → `CUP`+`SGR`+chars
(coalescing same-row runs, VGA→ANSI colour remap), `SYS_SET_CURSOR` → `CUP`,
`SYS_TTY_CLEAR` → `SGR`+`ED`+home — which `mxterm`'s vt100 core then renders.
`SYS_TERM_SIZE` reports the terminal's published size: `mxterm` calls
`SYS_PTY_WINSIZE` to stamp its grid dimensions onto the child's pipe. Grand-
children inherit the piped fd 1, so a TUI app launched from the shell in the
window works unchanged. The real text-VT path (non-NULL `vtty_buf_current()`) is
untouched, so console/Alt-Fn VTs behave exactly as before.

## makx protocol (`makx.h` / `makx.c`)

Control travels over the kernel's MINIX-style synchronous IPC (`kernel/ipc.h`,
32-byte messages); pixels travel over a shared surface. Endpoints are task pids;
the server passes each client `-makx <server-pid>` in argv, so a client finds
the server with no name service. A client launched **without** `-makx` (e.g. an
app typed into a GUI terminal) falls back to `sys_makx_server()` — the `$DISPLAY`
idiom — which returns the running `gui.elf` pid, so it connects and opens its own
window while its stdout keeps flowing to the terminal. (`doom` uses this: a GUI
terminal gives it a window; a true text console with no server returns 0 and it
runs fullscreen instead.) The server can't `wait4` a client it didn't fork, so it
reaps those windows by probing `kill(pid,0)` each frame and closing the window
when the client is gone.

Client → server requests (sent with `sys_ipc_sendrec`):

- `MX_HELLO(w,h,flags)` → reply `(win, sid)`: create a window + a `w×h` surface.
  `flags` is a bitmask: `MX_F_RESIZABLE` (the client re-flows to fill the window;
  the server blits 1:1 and sends `MXEV_RESIZE` instead of scaling) and
  `MX_F_RAWKEYS` (see *Keyboard delivery* below) and **`MX_F_DIALOG`** (see
  *Multi-window: dialogs* below). A **fixed-size** client (no
  `MX_F_RESIZABLE`, e.g. `doom` rendering a fixed internal frame) is scaled by the
  compositor to **fill the window preserving aspect ratio**, centred, with black
  letterbox/pillarbox bars — so drag-resize and maximize enlarge it instead of
  pinning it 1:1 in the corner. A game (`MX_F_RAWKEYS`) also opens enlarged (the
  largest integer multiple of its native frame that fits the desktop).
- `MX_PRESENT(win)` → reply = one input event: "I drew a frame, composite it."
- `MX_POLL(win)` → reply = one input event (drain input without presenting).
- `MX_BYE(win)` → ack + close that window slot.  Normally a client sends it as it
  exits (the reap loop also frees the slot); for a **dialog** window it closes
  just that window while the client keeps running (per-window teardown).
- `MX_WALLPAPER(sid,w,h)` → the client (mximg) hands the WM a decoded wallpaper
  as a shared surface; see *Desktop wallpaper* above.
- `MX_OPEN(sid,len)` → **default-app dispatch**: the client hands over a file path
  in a throwaway shared surface (it doesn't fit the IPC payload) and the WM opens
  it in the right app — images (`.png`/`.bmp`/`.jpg`/`.jpeg`/`.gif`) → `mximg`,
  `.htm`/`.html` → `mxweb`, `.elf` makx GUI apps (mx-prefixed, `gui`, `doom`) run
  directly, other executables in a terminal (`mxterm`), everything else in the
  editor (`mxedit`). The **WM is the launcher** so the opened window is its child
  and is reaped normally (a client-forked grandchild would ghost — the same
  parenting rule the Doom launcher relies on). Files double-clicked in `mxfiles`
  go through this.
- `MX_RELOAD_PREFS()` → ack: re-read `~/.mxrc` prefs (the dock's `Tray*` widgets)
  and repaint the desktop. Sent by `mxsettings` after a Status-Bar toggle so the
  change is live without a re-login; see *Settings (`mxsettings`)* above.

### Multi-window: dialogs

A client is normally one window, but it can open a **second, transient window**
— an open/save dialog — without becoming a new process.  `mx_open_window(dlg,
parent, w, h)` sends a fresh `MX_HELLO` with **`MX_F_DIALOG`** to the same
server; the WM **allocates a new, centred, focused window slot** for that client
(instead of the usual reuse of the client's existing window) and marks it a
dialog.  The app runs a small nested loop over the dialog's own `mx_conn`
(`mx_pump` → draw → `mx_present`) until the user accepts/cancels, then `mx_close`
sends a per-window `MX_BYE` that frees **just** that slot — the parent window is
untouched and keeps running (soft-modal: the parent isn't pumped meanwhile).
This is X11's transient-child idea on the makx protocol.

The shared file dialog rides on this: **`br_dialog_window(parent, b, mode, …)`**
(`gui_browser`) opens a dialog window and drives `br_dialog` in it, with a
**List / Icons** view toggle (the icon view is a folder/file glyph grid + a
scrollbar).  `mxedit` (Open / Save As), `mximg` (Open), `mxdoom` (Browse PWAD)
and `mxsettings` (Appearance → Browse…) all use it.

### Keyboard delivery (cooked vs raw)

By default `MXEV_KEY` carries a **cooked** byte (ASCII or a `KEY_*` sentinel) —
key-down only, the right thing for terminals, editors and text fields. A game
needs real key *up* events, so a client may set `MX_F_RAWKEYS` in its HELLO. While
such a client holds focus the WM switches the kernel keyboard to
scancode-passthrough (`sys_keyboard_raw(2)`) and forwards the raw set-1 make/break
scancode stream as the `MXEV_KEY` value (`low7 | 0x80` = break); on focus loss,
and around its own modal dialogs (login/power/passwd) and teardown, it restores
cooked mode. This is how windowed **DOOM** shares the exact input path its
fullscreen mode uses, with no synthesized releases. `Ctrl-Alt-Del` is detected
ahead of the passthrough in the kernel, so it still raises the power menu (and the
mash-twice hard reset) even while a raw-keys client is focused.

Each reply carries **one** input event (`MXEV_KEY/MOUSE/FOCUS/CLOSE/NONE`) plus
a "still pending" count in `data[MX_PENDING]`, so a client drains its input by
polling until the count is zero. **The server never sends a client an
unsolicited message** — it only ever replies — which keeps the synchronous
rendezvous deadlock-free: the server is purely reactive.

The kernel piece that makes this work is **`sys_ipc_nbrecv`** (non-blocking
receive, syscall 267): the server drains queued client requests *and* polls the
keyboard/mouse in the same loop, instead of parking in a blocking `ipc_recv`.
It drains a **bounded** number of requests per frame (a budget), then returns to
composite and `sys_yield` — an unbounded drain would spin forever on a couple of
busy-polling clients and starve everything else (including a not-yet-connected
client waiting to send its first HELLO). The bounded drain only stays fair
because the **kernel IPC sender queue is FIFO** (`ipc_send` appends at the tail):
a client polling every frame re-enqueues behind whatever is already waiting, so
it can't keep jumping the queue ahead of a newly launched app's one-shot HELLO.
(It was LIFO once — a busy `mximg` then starved a just-opened `mxfiles`, which
sat on "starting…" until `mximg` exited.)

The client library (`makx.c`) wraps this: `mx_connect` (parse `-makx`, HELLO,
map the surface into `c.surf`), `mx_pump` (drain events into `c`, compute mouse
edges), `mx_key` (pop a buffered key), `mx_present` (flush a frame), `mx_close`.

## Display server (`wm.c`)

`gui.elf` keeps a dynamic table of **client-backed** windows (`W[MAXWIN]`, each
= client pid + surface id + geometry + a small event queue) and a z-order list.
Each frame it: gathers mouse + one key; does window-management click handling
(dock, icons → `launch_icon`, raise+focus, title-bar drag, resize grip, min/max,
close box); forwards the key and client-relative mouse to the **focused**
window's event queue; drains client requests (`serve_requests`, bounded); reaps
exited clients (`reap_clients`, draining their stdout to prevent text-VT bleed);
then composites desktop → windows back-to-front (chrome + the client surface,
1:1 or `gfx_blit_scaled`) → dock → menu bar → cursor, and presents once.

**Compositing is damage-tracked.** Recompositing the scene is cheap (it lands in
a cacheable RAM back buffer), but *presenting* — copying to the framebuffer — is
the expensive step on a write-combining LFB, especially on VT-x hypervisors. So
the server accumulates a **damage rectangle** from the frame's actual changes
(a window moved/resized/redrew, a chrome/dock/menu band updated) and presents
**only that rect** via `SYS_FB_PRESENT_RECT` (269) instead of the whole screen.
The cursor is handled by a **save-under** fast path: on plain pointer motion with
nothing else dirty, the server restores the pixels under the old cursor box and
blits it at the new position — two tiny rect presents, no recomposite. And
**plain pointer motion is not forwarded to clients** at all (only clicks and
drags are), so idle hover over a window never makes that client repaint. Together
these keep multiple windows + Doom responsive on WC-framebuffer hosts, where a
full-screen present per mouse move was the bottleneck.

The server contains **no application logic** — it is a pure window server. It
launches a client by forking and `execve`-ing the icon's `.elf` with
`-makx <pid>` and a drained stdout/stderr pipe; the client's HELLO fills in the
reserved window's surface.

Clients (each an independent process, `src/userspace/mx*.c`):

- **mxterm** hosts `sh.elf` over pipes, forwarding the keys the server delivers
  to the shell's stdin. Ctrl-C / Ctrl-D there only closes that terminal —
  `mak.sh0` is never in the blast radius. It is a **real ANSI/VT100+ terminal
  emulator**: the child's byte stream runs through the reusable `vt100.c` core
  (see below), so colour (SGR), cursor addressing (CUP/ED/EL), scroll regions,
  insert/delete lines & chars, the alt-screen, and cursor show/hide all render
  correctly — TUI programs (`maktop`, `vix`, `ls --color`, …) display properly
  in the window. The grid resizes with the window.
- **mxedit** is a native multi-line editor (caret, click-to-position) with an
  **Open / Save / Save As** dialog built on the shared file dialog (`br_dialog`,
  below).
- **mxfiles** is a native browser on the same `browser` model: **Up / Open /
  Refresh** plus a path box + **Go**; `.`/`..` are hidden (Up handles the
  parent). (Cross-client "open in editor" is a follow-up; each client is
  self-contained for now.)
- **mxtasks** parses `/proc/tasks`, Kill via `SYS_KILL`, refresh-interval slider.
- **mxclock** draws a large digital time + date from `/proc/rtc` (GUI peer of the
  fullscreen `clock.elf`; reuses the same parse, scaled-glyph rendering).
- **mxcalc** is a button-grid calculator over the same integer expression
  evaluator as `calc.elf` (`+ - * / %`, parens, unary); mouse or keyboard input.
- **mxnet** shows the eth0/DHCP/DNS state (`SYS_NET_INFO`) with Renew / Release /
  Flush-DNS buttons (`SYS_NET_CTL`) — the GUI peer of `maknetcfg.elf`.
- **mxdisk** is a read-only view of the drives + partition table + FAT32 BPB
  (`SYS_DISK_INFO`), GUI peer of `diskinfo.elf` (destructive partitioning stays
  in `cfdisk`/`fdisk`).
- **mximg** is an image viewer: an Open dialog (shared `gui_browser`) or a path
  argument, decode, and aspect-fit-to-window via `gfx_blit_scaled`. Decodes BMP
  (24/32-bpp uncompressed), GIF (87a/89a first frame, LZW + interlace) and PNG
  (shared `img_png.c`: a from-scratch RFC1951 inflate + all five scanline
  filters, bit depths 1-16, colour types 0/2/3/4/6, non-interlaced — alpha
  discarded) and baseline JPEG (shared `img_jpg.c`: integer marker parse +
  Huffman + dequant + fixed-point IDCT + chroma upsample + YCbCr->RGB; 4:4:4 /
  4:2:2 / 4:2:0 + grayscale + restart markers, progressive rejected).
  mmap-backed file + pixel buffers (no libc). A **Gallery** toggle shows a
  scrollable thumbnail grid of images in `~/Pictures` + the bundled
  `/usr/share/pixmaps` and `/usr/share/backgrounds` (lazy aspect-fit thumbs);
  clicking one opens it in the viewer.
- **mxfiles** offers a **list view** (name + size + modified date via
  `sys_stat`) and an **icon view** (folder/file glyph grid) over the shared
  `gui_browser` model, toggled with the **View** button.
- **mxinstall** is the graphical OS installer (see "Installer" below).
- **doom** is the windowed makx client (see below).

`ui_password` fields render a small round dot per character (not `*`).

### Installer (`mxinstall.elf`)

The **Install** desktop icon launches `mxinstall.elf`, a graphical makx wizard:
Welcome → target drive → filesystem + optional components → hostname → accounts
(root + optional user) → confirm → progress → done. It collects an
`install_params_t` and drives the kernel's **shared, stepped install engine**
(`SYS_INSTALL_EXEC`: `install_exec_begin` / `install_exec_step` / `_finish`,
declared in `kernel/installer.h`) — one file copied per `step` over a
pre-counted total, so the client repaints a live percentage bar between files.
The same engine backs the text installer (`installer_run`, shell mode), so the
two front-ends are functionally identical; only the presentation differs.

The engine runs **preemptibly** (interrupts enabled), so the compositor keeps
running at full frame rate while files copy — the install never freezes the
desktop. Per-file progress is also written to the serial log
(`INSTALL>copy <n>/<total> (<pct>%) <name>`). Unlike a server install there is
**no auto-reboot**: the completion screen offers *Continue (live)* or *Reboot
now*, so on a live CD the operator stays in control (Ubuntu-style).

The text installer renders as a real ANSI/VT100 byte stream to its stdout, so it
also works from a shell VT and inside an `mxterm` window with one code path.

An always-on **top menu bar** (drawn after the windows, never occluded) carries
the Makar brand, macOS-style **Edit** / **View** dropdowns, the focused window's
title, and a **power icon** at the right.

**Desktop menus (Edit / View) + right-click context menu.** The menu-bar **Edit**
dropdown offers **Cut / Copy / Paste** and **View** offers **Auto Arrange**; a
**right-click on the empty desktop** opens the same actions as a context menu
(RCCM). Cut/Copy/Paste inject the `^X`/`^C`/`^V` byte into the focused client via
`win_push`, so the application's own clipboard handling runs exactly as if the
keystroke had been typed — there is no separate desktop clipboard and **no
undo/redo**. Auto Arrange re-grids every desktop icon (`icon_grid_pos`) and
persists the positions back to each `.desktop` (best-effort; a no-op on a
read-only live ISO). The edit verbs grey out when no window is focused. One
shared popup module (`menu_items`/`menu_box`/`draw_desk_menu`/`desk_menu_click`,
`wm.c`) backs both the menu-bar dropdowns and the RCCM, mirroring the dock
tray-menu pattern; `menubar_hit` opens a dropdown, an empty-desktop right-click
(gated off windows, icons and the dock) opens the RCCM.

Clicking the power icon (or pressing **Ctrl-Alt-Del**, see below) opens a centred
modal **power menu** (`show_power_menu`) with: **Log out (graphical)**
(→ `sys_logout`, re-shows login), **Log out to shell** (→ `sys_gui_close`,
back to the CLI shell), **Shut down** (→ `sys_shutdown`), **Reboot**
(→ `sys_reboot`), **Change password...** (the passwd dialog below), and
**Cancel** (Esc). Shut down / reboot and the two log-out paths all first
SIGKILL + reap every client child and restore statusbar state. Desktop + menu
bar + dock are unconditional: the GUI is never chromeless.

The **bottom dock** carries the open-window tabs on the left and a **system
tray** on the right — all status in one place: CPU/RAM load, a Vista-style
network indicator (green/amber/grey+X = connected/limited/down), and an HH:MM
clock + DD/MM/YY date (`draw_dock`/`tray_poll`/`draw_net_icon` in `wm.c`). The
top bar keeps only the brand, focused title, and the power icon.

**Change-password dialog** (`show_passwd_dialog`): a centred modal with masked
Current / New / Confirm fields (`ui_password`, Tab cycles, Esc cancels). OK
calls **`SYS_PASSWD`** (`sys_passwd`, syscall 270) → `shadow_verify` the old
password then `shadow_set_password` the new one; it needs a writable (installed)
rootfs and reports "current password incorrect" / mismatch inline.

**Ctrl-Alt-Del:** the kernel sets a pending flag from the keyboard IRQ; in a
text session it opens `cad_menu`, but under the GUI the server polls
**`SYS_CAD_PENDING`** (syscall 271, test-and-clear) every frame and opens the
power menu instantly. Escape hatch: a *second* Ctrl-Alt-Del within ~1 s pulses
an 8042 CPU reset from the IRQ, so a wedged GUI is still recoverable.

**Still built into the server (this cut):** the desktop/dock/menu-bar/chrome (a
compositor-owned panel + WM, like many simple stacks) and the graphical login
(it is tied to the session/auth handshake). Splitting the panel + login out to
their own clients is the remaining client-ization step (a server that composites
a pre-desktop fullscreen login client) — see Status.

### Graphical login

Launched as `gui login` (the kernel does this when no user is auto-logged-in),
`gui.elf` shows a graphical login (`do_login`): username + masked password
(`ui_password`) + a Log In button. Submit calls **`SYS_LOGIN`** (`sys_login`,
syscall 266) → `auth_login` → `shadow_verify` + set the session user; on success
it falls through to the desktop. Login stays *inside* the server for now (it is
tied to the session/auth handshake); lifting `do_login` into a standalone
`login.elf` client is Phase 6b (the server would composite a fullscreen login
client before opening the desktop).

### Booting straight to the desktop / login

The `autoboot=gui` kernel cmdline token makes `shell_login_loop` hand
authentication to the GUI: it tries autologin once, then passes
`--autostart=gui` (auto-logged-in → desktop) or `--autostart=gui-login` (→ the
graphical login) to the login `sh.elf`, which runs `gui` / `gui login` after
sourcing `~/.makshrc`. The desktop runs inside the login session (which stays
the GUI's parent, so Log Off returns to it cleanly).

Bootloader entries:
- **GRUB (live ISO):** `Makar OS` / `Makar OS (GUI desktop)` (`live autoboot=gui`,
  the **default** entry) / `rescue shell` / `serial console`. `GRUB_DEFAULT`
  overrides the auto-selected entry (the kbtest/guitest harnesses pin it to 0).
- **Limine (installed):** the installer builds `limine.conf` with a
  `/Makar OS (GUI desktop)` entry whose cmdline embeds the configured autologin
  user (`autologin=<user> autoboot=gui`), plus a rescue entry.

### Testing the GUI boots: `./run.sh guitest`

Boots straight into the desktop (autologin), waits for `gui.elf`'s `GUI: READY`
serial marker (emitted after the first composited frame is presented), captures
a QEMU **screendump** (PPM → `gui-screendump.bmp` via `tests/ppm2bmp.py`), then
shuts down. The marker is the pass gate; the screendump is a best-effort viewable
artifact. This is the end-to-end "the window server actually starts and draws"
check that the headless `uitest`/`fstest` (which never touch the framebuffer)
cannot give.

### Reusable file browser + headless coverage

The Files client and the Editor's Open/Save dialog share one `browser` model
*and* one **shared file dialog** — `br_dialog` in `gui_browser.{c,h}` draws the
Up/Open|Save/Cancel toolbar + optional Name field + listbox into any surface rect
and returns accept/cancel, so any windowed app can drop in the same open/save
picker (the editor uses it; a future "save page" in a browser would too). The
model navigates via the existing `chdir`/`readdir`/`getcwd` syscalls.
`gui.elf fstest` drives that model against the
live VFS (descend a real dir, confirm the cwd deepened, climb back) and emits
`GUI-FSTEST: PASS`; it runs under `shell-smoke.sh` alongside `gui uitest`
(`GUI-UITEST: PASS`). Both return before touching the framebuffer, so they are
headless-safe — this is the automated proof that "Files actually moves about the
filesystem" without needing pixels.

## Text-mode VTs, /dev/ttyN, and the per-tab status bar

Outside the GUI, the text console runs `makmux` — a tmux-style virtual-terminal
multiplexer (`src/userspace/makmux.c`). There are nine user VT slots plus a
hidden root console; full model in [kernel/vtty](kernel/vtty.md). The relevant
points for the desktop story:

- **/dev/ttyN.** Every VT slot is addressable as a Linux-style character node:
  `/dev/tty0` is the root console (mak.sh0) and `/dev/tty1`..`/dev/tty9` are the
  nine makmux slots (slot `i` == `/dev/tty(i+1)`). Writing to one streams into
  that VT's backing grid (`echo hi > /dev/tty2`); reads are EOF. They are always
  present and ride the existing devfs `SYS_OPEN`/`SYS_WRITE` path — no new
  syscall.
- **tmux-style tabs.** makmux opens **one** shell by default; further tabs are
  created on demand with **Alt+T** (`SYS_VT_OPEN_REQUEST`), up to nine. Alt+F1–F4
  jump to the first four; Alt+Tab / Ctrl+Tab cycle the rest.
- **Per-tab status bar.** `statusbar.elf` renders the layout of the *active* VT
  (read from `SYS_VT_STATE`). Each tab can own its bar via `~/.sbrc.tty<N>`
  (the `/dev/ttyN` number; root = `tty0`), which overrides the shared `~/.sbrc`;
  switching tabs reloads and re-renders that tab's own bar. Layout/widget format
  is the same `<section> <widget>...` `.sbrc` grammar (`left`/`center`/`right`
  with `hostname user date time cpu mem rootfs command tabs`).

## DOOM windowed backend (`doomgeneric_makar.c`)

`-makx <pid>` selects windowed mode: `main` calls `mx_connect` (640×400 surface)
before `doomgeneric_Create`; `DG_Init` renders into `mx_conn.surf` instead of the
full framebuffer; `DG_DrawFrame` writes the frame into the surface and
`mx_present`s it (never `SYS_FB_PRESENT` — the server composites); input is the
keys `mx_pump` delivers (not the raw scancode stream). Since makx keys are
press-only, a press auto-releases after ~120 ms (tap-to-move) — good enough for
menus/turning, a known limitation. Without `-makx`, DOOM runs its normal
fullscreen path (shell `doom`), unchanged — so DOOM works the same in GUI and
text mode. Only `makx.o` is linked in (no `gui_gfx`): the backend uses the
`gfx_surface` *type* from `makx.h`, not any drawing code.

`convertToDoomKey` translates set-1 scancodes to Doom keys for **both** paths.
Besides the movement/action bindings it maps the full **a–z / 0–9** rows to
ASCII, so the engine's built-in **cheat responder** (`iddqd`, `idkfa`, `idclip`,
`idspispopd`, `idbehold`, `idclev##`, `idmus##`, `idchoppers`) and save-game name
entry receive the letters — type them in-game the same as on a PC.

## Status

- **Phase 1 (done):** kernel shared surfaces + ktest.
- **Phase 2 (done):** userspace GUI widget framework (`gui_gfx`, `gui_ui`).
- **Phase 3 (done):** multi-window WM + click-to-focus input routing.
- **Phase 4 (done):** native Editor / Files / Task-manager windows, now with a
  working file browser (Up/Open/Go), an Editor Open/Save/Save-As dialog on the
  shared browser, and a dock **Log Off**. Navigation is regression-covered by
  `gui fstest` (`GUI-FSTEST: PASS`).
- **Phase 5 (done):** DOOM in a window via a shared surface.
- **Phase 6 — makx server/client split (done):** `gui.elf` is now a pure
  display server; every application (terminal, files, editor, tasks, doom) is an
  independent client process talking the makx protocol (`makx.h`) over IPC +
  shared surfaces. Added one kernel syscall (`sys_ipc_nbrecv`, 267). The shared
  file dialog (`br_dialog`, in `gui_browser`) is reusable by any client.
- **Phase 6b (todo):** lift the panel (dock + menu bar) and the login screen out
  to their own clients (the server would composite a pre-desktop fullscreen login
  client; the panel becomes a normal always-on-top client window).
- **Phase 7 (ideas / roadmap):** more makx clients now that the protocol exists —
  a lynx/dillo-style text web browser (`mxweb`, fetch via `sys_wget` → render),
  an image viewer (BMP/PNG/JPEG/GIF), a simple paint app. See `CLAUDE.roadmap.md`.
- **GUI designer (deferred):** host drag-and-drop designer emitting widget-layout
  code. The widget schema (`gui_ui.h`) is the contract it will target.

## Next PR (not this one): trim the syscall surface

The GUI leans on many bespoke syscalls (`FB_PRESENT`, `DRAW_LINE`, `MOUSE_READ`,
`SURFACE_*`, `WHOAMI`, `STATUSBAR`, `WRITE_FILE`, `WRITE_SERIAL`, `KEYBOARD_RAW`)
that on real Linux are device files + ioctl (`/dev/fb0`, `/dev/input/*`), `getuid`,
`open`/`write`, `/dev/kmsg`, termios. Reducing the ~101-syscall surface toward
those idioms is the next "proper kernel" PR; GUI changes here were kept
syscall-neutral so that rework isn't pre-empted.

## Verification status

- **Navigation** (Files / Editor dialog) is headlessly proven by `gui fstest`.
- **Widgets** by `gui uitest`.
- **The desktop boots and draws** by `./run.sh guitest` — `GUI: READY` plus a
  1280×720 screendump whose colours are exactly the WM palette (desktop bg,
  window, title bar, menu bar, icons).

Still wanting a human pass (inherently interactive): click-to-focus *feel*, the
Editor Save-As dialog, the graphical login keyboard flow, and the Doom surface
blit/playability. `./run.sh iso boot` and pick **Makar OS (GUI desktop)**.
