/*
 * video_svga2.c -- VMware / VirtualBox SVGA II display driver (PCI 15ad:0405).
 *
 * On these adapters the linear framebuffer the bootloader handed us is the
 * SVGA FB, but the host only refreshes regions the guest explicitly marks with
 * a FIFO SVGA_CMD_UPDATE -- a plain LFB write may never reach the screen.  This
 * driver reuses that FB (no mode change, so vesa_tty stays valid), brings up the
 * command FIFO, and on present() does the CPU copy then issues an UPDATE for the
 * dirty rect.  It also drives the device's hardware cursor sprite.
 *
 * Anything unexpected during bring-up returns non-zero from init() so the
 * framework falls back to the dumb LFB driver -- SVGA is never load-bearing for
 * boot.  Reference: VMware's published SVGA device spec / svga_reg.h.
 */
#include <kernel/video.h>
#include <kernel/vesa.h>
#include <kernel/pci.h>
#include <kernel/paging.h>
#include <kernel/serial.h>
#include <kernel/asm.h>
#include <string.h>

/* PCI identity. */
#define SVGA_VENDOR_ID  0x15ADu
#define SVGA_DEVICE_ID  0x0405u

/* I/O port offsets from BAR0 (32-bit index/value pair). */
#define SVGA_INDEX_PORT 0u
#define SVGA_VALUE_PORT 1u

/* Registers (svga_reg.h subset). */
#define SVGA_REG_ID              0u
#define SVGA_REG_ENABLE          1u
#define SVGA_REG_WIDTH           2u
#define SVGA_REG_HEIGHT          3u
#define SVGA_REG_MAX_WIDTH       4u
#define SVGA_REG_MAX_HEIGHT      5u
#define SVGA_REG_BITS_PER_PIXEL  7u
#define SVGA_REG_BYTES_PER_LINE  12u
#define SVGA_REG_FB_START        13u
#define SVGA_REG_FB_OFFSET       14u
#define SVGA_REG_VRAM_SIZE       15u
#define SVGA_REG_FB_SIZE         16u
#define SVGA_REG_CAPABILITIES    17u
#define SVGA_REG_FIFO_START      18u
#define SVGA_REG_FIFO_SIZE       19u
#define SVGA_REG_CONFIG_DONE     20u
#define SVGA_REG_SYNC            21u
#define SVGA_REG_BUSY            22u
#define SVGA_REG_CURSOR_ID       24u
#define SVGA_REG_CURSOR_X        25u
#define SVGA_REG_CURSOR_Y        26u
#define SVGA_REG_CURSOR_ON       27u

#define SVGA_ID_2   0x90000002u   /* magic | version 2 */

/* FIFO layout: the first words are control registers (32-bit word indices). */
#define SVGA_FIFO_MIN           0u
#define SVGA_FIFO_MAX           1u
#define SVGA_FIFO_NEXT_CMD      2u
#define SVGA_FIFO_STOP          3u
/* Extended FIFO registers.  Usable only when the driver reserves space for them
 * below SVGA_FIFO_MIN *and* the matching capability bit is set in
 * SVGA_FIFO_CAPABILITIES.  The cursor-bypass registers are how VMware/VBox
 * VMSVGA position the hardware cursor -- the legacy SVGA_REG_CURSOR_* index
 * registers are ignored on those devices. */
#define SVGA_FIFO_CAPABILITIES        4u
#define SVGA_FIFO_FLAGS               5u
#define SVGA_FIFO_FENCE               6u
#define SVGA_FIFO_CURSOR_ON           9u
#define SVGA_FIFO_CURSOR_X            10u
#define SVGA_FIFO_CURSOR_Y            11u
#define SVGA_FIFO_CURSOR_COUNT        12u
#define SVGA_FIFO_CURSOR_LAST_UPDATED 13u
/* Reserve through the cursor registers so the device treats words 0..15 as
 * registers; the command ring then starts at SVGA_FIFO_MIN = NUM_REGS * 4. */
#define SVGA_FIFO_NUM_REGS      16u

/* SVGA_FIFO_CAPABILITIES bits. */
#define SVGA_FIFO_CAP_CURSOR_BYPASS_3  (1u << 4)

/* FIFO commands. */
#define SVGA_CMD_UPDATE              1u
#define SVGA_CMD_DEFINE_ALPHA_CURSOR 22u

/* Cursor visibility states. */
#define SVGA_CURSOR_ON_HIDE 0u
#define SVGA_CURSOR_ON_SHOW 1u

#define SVGA_CURSOR_MAXSZ   64

static uint16_t   s_io;
static volatile uint32_t *s_fifo;
static uint32_t   s_fifo_words;
static pci_device_t *s_dev;
static int        s_up;
static int        s_cursor_bypass;   /* device honours FIFO cursor-bypass regs */

/* Last-defined cursor, cached so it can be re-uploaded after a mode switch:
 * toggling SVGA_REG_ENABLE in svga_set_mode() drops the device's cursor state,
 * so without this the sprite disappears when the resolution changes. */
static uint32_t s_cur_argb[SVGA_CURSOR_MAXSZ * SVGA_CURSOR_MAXSZ];
static int      s_cur_w, s_cur_h, s_cur_hx, s_cur_hy, s_cur_valid;
static int      s_cur_x, s_cur_y, s_cur_on;

static void cursor_upload(void);
static void svga_cursor_move(int x, int y);
static void svga_cursor_show(int on);

static uint32_t reg_read(uint32_t index)
{
    outl(s_io + SVGA_INDEX_PORT, index);
    return inl(s_io + SVGA_VALUE_PORT);
}
static void reg_write(uint32_t index, uint32_t value)
{
    outl(s_io + SVGA_INDEX_PORT, index);
    outl(s_io + SVGA_VALUE_PORT, value);
}

static void svga_sync(void)
{
    /* Bounded spin: a healthy host drains the FIFO and clears BUSY almost
     * immediately.  Cap the wait so a mis-configured FIFO (e.g. stale ring
     * registers after a mode switch) degrades to a dropped sync rather than
     * hanging the CPU with interrupts disabled -- which presents as the guest
     * "halting" with no fault. */
    reg_write(SVGA_REG_SYNC, 1);
    for (uint32_t spins = 0; spins < 100000000u && reg_read(SVGA_REG_BUSY); spins++)
        ;
}

/* Append one word to the command FIFO, syncing the host if it's full. */
static void fifo_write(uint32_t value)
{
    uint32_t min  = s_fifo[SVGA_FIFO_MIN];
    uint32_t max  = s_fifo[SVGA_FIFO_MAX];
    uint32_t next = s_fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t stop = s_fifo[SVGA_FIFO_STOP];

    /* If the ring is full (next+1 == stop, modulo wrap), drain it. */
    uint32_t nplus = next + 4u;
    if (nplus == max) nplus = min;
    if (nplus == stop) {
        svga_sync();
        stop = s_fifo[SVGA_FIFO_STOP];
        (void)stop;
    }

    s_fifo[next / 4u] = value;
    next += 4u;
    if (next == max) next = min;
    s_fifo[SVGA_FIFO_NEXT_CMD] = next;
}

static int svga_probe(void)
{
    /* Reuse the bootloader LFB (we don't mode-set), so a framebuffer must exist. */
    if (!vesa_get_fb())
        return 0;
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == SVGA_VENDOR_ID &&
            pci_devices[i].device_id == SVGA_DEVICE_ID) {
            s_dev = &pci_devices[i];
            return 1;
        }
    }
    return 0;
}

/* Map the command FIFO, initialise the ring registers, mark CONFIG_DONE and
 * (re)detect cursor-bypass support.  Run at init *and* after every mode switch:
 * toggling SVGA_REG_ENABLE drops the device's FIFO state, so the ring must be
 * re-established before the next fifo_write/svga_sync or the host stops draining
 * it (which hangs svga_sync).  Returns 0 on success, -1 if the FIFO is absent. */
static int fifo_bringup(void)
{
    uint32_t fifo_phys  = reg_read(SVGA_REG_FIFO_START);
    uint32_t fifo_bytes = reg_read(SVGA_REG_FIFO_SIZE);
    if (!fifo_phys || fifo_bytes < SVGA_FIFO_NUM_REGS * 4u + 16u)
        return -1;

    paging_map_region(fifo_phys, fifo_bytes);
    s_fifo = (volatile uint32_t *)(uintptr_t)fifo_phys;
    s_fifo_words = fifo_bytes / 4u;

    s_fifo[SVGA_FIFO_MIN]      = SVGA_FIFO_NUM_REGS * 4u;
    s_fifo[SVGA_FIFO_MAX]      = fifo_bytes;
    s_fifo[SVGA_FIFO_NEXT_CMD] = SVGA_FIFO_NUM_REGS * 4u;
    s_fifo[SVGA_FIFO_STOP]     = SVGA_FIFO_NUM_REGS * 4u;
    reg_write(SVGA_REG_CONFIG_DONE, 1);

    /* The HW cursor is positioned through the FIFO cursor-bypass registers when
     * the device advertises the capability (VMware/VBox VMSVGA); the legacy
     * SVGA_REG_CURSOR_X/Y index registers are ignored there, which left the
     * sprite stuck at (0,0).  Falls back to the legacy registers otherwise. */
    s_cursor_bypass =
        (s_fifo[SVGA_FIFO_CAPABILITIES] & SVGA_FIFO_CAP_CURSOR_BYPASS_3) ? 1 : 0;
    Serial_WriteString("svga2: cursor bypass ");
    Serial_WriteString(s_cursor_bypass ? "on\n" : "off (legacy regs)\n");
    return 0;
}

/* Snap a requested geometry to the largest *clean standard* mode the device and
 * VRAM actually support (<= req, <= MAX_WIDTH/HEIGHT, fits VRAM).  Programming a
 * non-standard combo is the trap: ask VBox VMSVGA (MAX_WIDTH 1280) for
 * 1920x1080 and it clamps the width but honours the height -> a mismatched
 * 1280x1080.  Picking from a list of real modes keeps the aspect sane: VMware
 * (max 4K) yields 1920x1080, VBox (max 1280) yields a clean 1280x720. */
static void svga_pick_mode(uint32_t req_w, uint32_t req_h,
                           uint32_t *out_w, uint32_t *out_h)
{
    uint32_t max_w = reg_read(SVGA_REG_MAX_WIDTH);
    uint32_t max_h = reg_read(SVGA_REG_MAX_HEIGHT);
    uint32_t vram  = reg_read(SVGA_REG_VRAM_SIZE);
    static const struct { uint32_t w, h; } cand[] = {
        { 1920, 1080 }, { 1600, 900 }, { 1366, 768 }, { 1280, 720 },
        { 1024, 768 },  { 800, 600 },  { 640, 480 },
    };
    for (unsigned i = 0; i < sizeof(cand)/sizeof(cand[0]); i++) {
        if (cand[i].w > req_w || cand[i].h > req_h)                 continue;
        if (max_w && cand[i].w > max_w)                            continue;
        if (max_h && cand[i].h > max_h)                            continue;
        if (vram && (uint32_t)cand[i].w * cand[i].h * 4u > vram)   continue;
        *out_w = cand[i].w; *out_h = cand[i].h; return;
    }
    *out_w = 640; *out_h = 480;   /* universal fallback */
}

static int svga_init(void)
{
    if (!s_dev)
        return -1;
    s_io = (uint16_t)pci_bar_io(s_dev, 0);
    if (!s_io)
        return -1;

    /* Negotiate version 2; bail to the LFB driver if the device disagrees. */
    reg_write(SVGA_REG_ID, SVGA_ID_2);
    if (reg_read(SVGA_REG_ID) != SVGA_ID_2)
        return -1;

    pci_enable_bus_master(s_dev);

    /* Mode-set, then adopt the FB base + stride the device reports.  Enabling
     * SVGA without a mode-set drops into an unconfigured 0x0 SVGA mode (blank
     * scanout), so an explicit WIDTH/HEIGHT/BPP is required.
     *
     * Pick the resolution from the device's own capabilities rather than the
     * geometry the boot VBE left us in: VirtualBox/VMware expose only a low mode
     * through their Bochs-VBE compat layer, but the SVGA registers here set
     * arbitrary modes.  Default to the adapter's advertised maximum
     * (SVGA_REG_MAX_WIDTH/HEIGHT); an explicit vmode= request overrides it.
     * Bound by VRAM so a max-by-max frame can't exceed what the adapter has. */
    uint32_t max_w = reg_read(SVGA_REG_MAX_WIDTH);    /* for the diagnostic log */
    uint32_t max_h = reg_read(SVGA_REG_MAX_HEIGHT);
    /* Requested geometry: an explicit vmode= (capped at the 1080p ceiling) or
     * the 720p default (1080p is too large by default; vmode=/the display app
     * can still ask for it).  svga_pick_mode snaps it to a clean mode the
     * device and VRAM support. */
    extern uint32_t g_video_pref_w, g_video_pref_h;
    uint32_t req_w = 1280, req_h = 720;
    if (g_video_pref_w && g_video_pref_h) {
        req_w = g_video_pref_w > 1920 ? 1920 : g_video_pref_w;
        req_h = g_video_pref_h > 1080 ? 1080 : g_video_pref_h;
    }
    uint32_t want_w, want_h;
    svga_pick_mode(req_w, req_h, &want_w, &want_h);

    reg_write(SVGA_REG_ENABLE, 0);
    reg_write(SVGA_REG_WIDTH, want_w);
    reg_write(SVGA_REG_HEIGHT, want_h);
    reg_write(SVGA_REG_BITS_PER_PIXEL, 32);
    reg_write(SVGA_REG_ENABLE, 1);

    uint32_t fb_phys  = reg_read(SVGA_REG_FB_START);
    uint32_t fb_off   = reg_read(SVGA_REG_FB_OFFSET);
    uint32_t pitch    = reg_read(SVGA_REG_BYTES_PER_LINE);
    uint32_t got_w    = reg_read(SVGA_REG_WIDTH);
    uint32_t got_h    = reg_read(SVGA_REG_HEIGHT);
    if (!fb_phys || !pitch || !got_w || !got_h) {
        reg_write(SVGA_REG_ENABLE, 0);
        return -1;
    }

    /* Map the FB (write-combining), the same convention vesa uses for the
     * adopted bootloader LFB; the FIFO mapping + ring init is in fifo_bringup. */
    paging_map_region_wc(fb_phys, fb_off + pitch * got_h);
    if (fifo_bringup() != 0) {
        reg_write(SVGA_REG_ENABLE, 0);
        return -1;
    }

    /* Repoint the kernel's framebuffer at the SVGA FB. */
    vesa_set_framebuffer((uint32_t *)(uintptr_t)(fb_phys + fb_off),
                         pitch, got_w, got_h, 32);

    Serial_WriteString("svga2: ");
    Serial_WriteDec(got_w); Serial_WriteString("x"); Serial_WriteDec(got_h);
    Serial_WriteString(" (dev max "); Serial_WriteDec(max_w);
    Serial_WriteString("x"); Serial_WriteDec(max_h);
    Serial_WriteString(", vram "); Serial_WriteDec(reg_read(SVGA_REG_VRAM_SIZE) >> 20);
    Serial_WriteString("MB)");
    Serial_WriteString(" fb "); Serial_WriteHex(fb_phys + fb_off);
    Serial_WriteString(" pitch "); Serial_WriteDec(pitch);
    Serial_WriteString(" fifo "); Serial_WriteHex((uint32_t)(uintptr_t)s_fifo);
    Serial_WriteString("\n");
    s_up = 1;
    return 0;
}

static void svga_present_rect(const void *src, uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h)
{
    const vesa_fb_t *fb = vesa_get_fb();
    if (!fb || !src || !s_up)
        return;
    if (x >= fb->width || y >= fb->height)
        return;
    if (x + w > fb->width)  w = fb->width  - x;
    if (y + h > fb->height) h = fb->height - y;
    if (!w || !h)
        return;

    /* CPU-copy the dirty rect into the SVGA FB ... */
    const uint8_t *s = (const uint8_t *)src;
    uint8_t       *d = (uint8_t *)fb->addr;
    uint32_t src_pitch  = fb->width * 4u;
    uint32_t copy_bytes = w * 4u;
    for (uint32_t row = 0; row < h; row++) {
        uint32_t line = y + row;
        memcpy(d + line * fb->pitch + x * 4u,
               s + line * src_pitch + x * 4u, copy_bytes);
    }

    /* ... then tell the host to scan it out. */
    fifo_write(SVGA_CMD_UPDATE);
    fifo_write(x);
    fifo_write(y);
    fifo_write(w);
    fifo_write(h);
}

/* Scan out a rect already written into the FB by someone else (the text
 * console paints fb->addr directly): no copy, just the UPDATE command -- the
 * device won't show direct writes otherwise. */
static void svga_flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    const vesa_fb_t *fb = vesa_get_fb();
    if (!fb || !s_up)
        return;
    if (x >= fb->width || y >= fb->height)
        return;
    if (x + w > fb->width)  w = fb->width  - x;
    if (y + h > fb->height) h = fb->height - y;
    if (!w || !h)
        return;

    fifo_write(SVGA_CMD_UPDATE);
    fifo_write(x);
    fifo_write(y);
    fifo_write(w);
    fifo_write(h);
}

/* Runtime mode switch (setmode / the display app).  VMSVGA sets arbitrary modes
 * through its registers, which the Bochs DISPI path can't -- so route through
 * here when this driver is bound.  Caps at 1080p and the device max, then
 * repoints the framebuffer; the caller re-fits the text console and the WM
 * reflows itself off the new geometry. */
static int svga_set_mode(uint32_t w, uint32_t h)
{
    if (!s_up)
        return -1;
    if (w > 1920) w = 1920;
    if (h > 1080) h = 1080;
    /* Snap to a clean standard mode the device + VRAM support (see
     * svga_pick_mode) so a 1080p request on a 1280-max VBox adapter lands on
     * 1280x720, not a mangled 1280x1080. */
    { uint32_t pw, ph; svga_pick_mode(w, h, &pw, &ph); w = pw; h = ph; }

    reg_write(SVGA_REG_ENABLE, 0);
    reg_write(SVGA_REG_WIDTH, w);
    reg_write(SVGA_REG_HEIGHT, h);
    reg_write(SVGA_REG_BITS_PER_PIXEL, 32);
    reg_write(SVGA_REG_ENABLE, 1);

    uint32_t fb_phys = reg_read(SVGA_REG_FB_START);
    uint32_t fb_off  = reg_read(SVGA_REG_FB_OFFSET);
    uint32_t pitch   = reg_read(SVGA_REG_BYTES_PER_LINE);
    uint32_t gw = reg_read(SVGA_REG_WIDTH);
    uint32_t gh = reg_read(SVGA_REG_HEIGHT);
    if (!fb_phys || !pitch || !gw || !gh)
        return -1;

    paging_map_region_wc(fb_phys, fb_off + pitch * gh);
    vesa_set_framebuffer((uint32_t *)(uintptr_t)(fb_phys + fb_off), pitch, gw, gh, 32);

    /* Re-establish the command FIFO: toggling SVGA_REG_ENABLE above drops the
     * device's ring state, and on VMware the host stops draining the stale ring
     * -- so the next svga_sync() would spin forever (the guest appears to halt).
     * Re-bring-up before any fifo_write/cursor_upload below. */
    if (fifo_bringup() != 0)
        return -1;

    /* Toggling SVGA_REG_ENABLE above also dropped the device's cursor; redraw
     * the cached sprite and restore its position/visibility so the pointer
     * doesn't vanish after a resolution change. */
    cursor_upload();
    svga_cursor_move(s_cur_x, s_cur_y);
    svga_cursor_show(s_cur_on);
    return 0;
}

/* Push the cached cursor sprite to the device via the FIFO define command.
 * Used both by svga_cursor_define and after a mode switch (which resets the
 * device cursor) to redraw the sprite. */
static void cursor_upload(void)
{
    if (!s_up || !s_cur_valid)
        return;
    fifo_write(SVGA_CMD_DEFINE_ALPHA_CURSOR);
    fifo_write(0);                 /* cursor id */
    fifo_write((uint32_t)s_cur_hx);
    fifo_write((uint32_t)s_cur_hy);
    fifo_write((uint32_t)s_cur_w);
    fifo_write((uint32_t)s_cur_h);
    for (int i = 0; i < s_cur_w * s_cur_h; i++)
        fifo_write(s_cur_argb[i]); /* premultiplied BGRA, one dword/pixel */
    svga_sync();
    reg_write(SVGA_REG_CURSOR_ID, 0);
}

static int svga_cursor_define(const uint32_t *argb, int w, int h,
                              int hot_x, int hot_y)
{
    if (!s_up || !argb || w <= 0 || h <= 0 ||
        w > SVGA_CURSOR_MAXSZ || h > SVGA_CURSOR_MAXSZ)
        return -1;
    /* Cache the sprite so it can be re-uploaded after a mode switch. */
    memcpy(s_cur_argb, argb, (size_t)w * (size_t)h * 4u);
    s_cur_w = w; s_cur_h = h; s_cur_hx = hot_x; s_cur_hy = hot_y;
    s_cur_valid = 1;
    cursor_upload();
    return 0;
}

/* Position/show the sprite through *every* protocol the device might honour:
 * the FIFO cursor-bypass-3 registers (VBox VMSVGA ignores the legacy index
 * registers, so it needs these) and the legacy SVGA_REG_CURSOR_* index
 * registers (some VMware builds don't latch from the FIFO bypass words, so
 * they need these).  Writing both is harmless -- a device honours one path and
 * ignores the other -- and lets one driver track the cursor on both hosts
 * without per-host detection of which path actually works. */
static void svga_cursor_move(int x, int y)
{
    if (!s_up) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    s_cur_x = x; s_cur_y = y; s_cur_on = 1;
    if (s_cursor_bypass) {
        /* Bypass-3: write X/Y/ON, then bump CURSOR_COUNT so the device latches
         * the new position (the exact vmwgfx update sequence). */
        s_fifo[SVGA_FIFO_CURSOR_X]     = (uint32_t)x;
        s_fifo[SVGA_FIFO_CURSOR_Y]     = (uint32_t)y;
        s_fifo[SVGA_FIFO_CURSOR_ON]    = SVGA_CURSOR_ON_SHOW;
        s_fifo[SVGA_FIFO_CURSOR_COUNT] = s_fifo[SVGA_FIFO_CURSOR_COUNT] + 1u;
    }
    /* Legacy cursor-bypass registers (also harmless on bypass-3 devices). */
    reg_write(SVGA_REG_CURSOR_ID, 0);
    reg_write(SVGA_REG_CURSOR_X,  (uint32_t)x);
    reg_write(SVGA_REG_CURSOR_Y,  (uint32_t)y);
    reg_write(SVGA_REG_CURSOR_ON, SVGA_CURSOR_ON_SHOW);
}

static void svga_cursor_show(int on)
{
    if (!s_up) return;
    s_cur_on = on ? 1 : 0;
    if (s_cursor_bypass) {
        s_fifo[SVGA_FIFO_CURSOR_ON]    = on ? SVGA_CURSOR_ON_SHOW : SVGA_CURSOR_ON_HIDE;
        s_fifo[SVGA_FIFO_CURSOR_COUNT] = s_fifo[SVGA_FIFO_CURSOR_COUNT] + 1u;
    }
    reg_write(SVGA_REG_CURSOR_ON, on ? SVGA_CURSOR_ON_SHOW : SVGA_CURSOR_ON_HIDE);
}

/* Panic / fallback path: take the device out of SVGA mode so the legacy VGA
 * text buffer (0xB8000) is scanned out again -- the caller then programs VGA
 * mode 3 and writes the panic there.  At panic time we must not depend on the
 * command FIFO or the high-res framebuffer (which may be the very thing that
 * broke), so this only pokes the index/value registers.  No-op if the driver
 * never bound (s_io == 0). */
void video_svga2_to_vga(void)
{
    if (!s_io)
        return;
    reg_write(SVGA_REG_ENABLE, 0);
}

const vid_driver_t video_svga2 = {
    /* The SVGA II / VMSVGA protocol is presented by both VMware and VirtualBox
     * (and QEMU's -device vmware-svga), so the driver name is platform-neutral;
     * the actual host shows up separately as the "hypervisor" line in
     * /proc/cpuinfo (and the about command).  Mirrors Linux using one vmwgfx
     * driver for the device on every host. */
    .name          = "svga-ii",
    .probe         = svga_probe,
    .init          = svga_init,
    .set_mode      = svga_set_mode,
    .present_rect  = svga_present_rect,
    .flush_rect    = svga_flush_rect,
    .cursor_define = svga_cursor_define,
    .cursor_move   = svga_cursor_move,
    .cursor_show   = svga_cursor_show,
    .caps          = VID_CAP_HW_CURSOR,
};
