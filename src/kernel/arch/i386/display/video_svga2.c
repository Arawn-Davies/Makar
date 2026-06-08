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
#define SVGA_REG_BITS_PER_PIXEL  7u
#define SVGA_REG_BYTES_PER_LINE  12u
#define SVGA_REG_FB_START        13u
#define SVGA_REG_FB_OFFSET       14u
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
    reg_write(SVGA_REG_SYNC, 1);
    while (reg_read(SVGA_REG_BUSY))
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

    /* Mode-set to the geometry the rest of the kernel already uses, then adopt
     * the FB base + stride the device reports.  Enabling SVGA without a mode-set
     * drops the VBE mode into an unconfigured 0x0 SVGA mode (blank scanout), so
     * the explicit WIDTH/HEIGHT/BPP is required. */
    const vesa_fb_t *cur = vesa_get_fb();
    uint32_t want_w = cur->width, want_h = cur->height;

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

    uint32_t fifo_phys  = reg_read(SVGA_REG_FIFO_START);
    uint32_t fifo_bytes = reg_read(SVGA_REG_FIFO_SIZE);
    if (!fifo_phys || fifo_bytes < SVGA_FIFO_NUM_REGS * 4u + 16u) {
        reg_write(SVGA_REG_ENABLE, 0);
        return -1;
    }

    /* Map the FB (write-combining) and the FIFO (uncached) identity, the same
     * convention vesa uses for the adopted bootloader LFB. */
    paging_map_region_wc(fb_phys, fb_off + pitch * got_h);
    paging_map_region(fifo_phys, fifo_bytes);
    s_fifo = (volatile uint32_t *)(uintptr_t)fifo_phys;
    s_fifo_words = fifo_bytes / 4u;

    /* Initialise the command ring. */
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

    /* Repoint the kernel's framebuffer at the SVGA FB. */
    vesa_set_framebuffer((uint32_t *)(uintptr_t)(fb_phys + fb_off),
                         pitch, got_w, got_h, 32);

    Serial_WriteString("svga2: ");
    Serial_WriteDec(got_w); Serial_WriteString("x"); Serial_WriteDec(got_h);
    Serial_WriteString(" fb "); Serial_WriteHex(fb_phys + fb_off);
    Serial_WriteString(" pitch "); Serial_WriteDec(pitch);
    Serial_WriteString(" fifo "); Serial_WriteHex(fifo_phys);
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

static int svga_cursor_define(const uint32_t *argb, int w, int h,
                              int hot_x, int hot_y)
{
    if (!s_up || !argb || w <= 0 || h <= 0 ||
        w > SVGA_CURSOR_MAXSZ || h > SVGA_CURSOR_MAXSZ)
        return -1;
    fifo_write(SVGA_CMD_DEFINE_ALPHA_CURSOR);
    fifo_write(0);                 /* cursor id */
    fifo_write((uint32_t)hot_x);
    fifo_write((uint32_t)hot_y);
    fifo_write((uint32_t)w);
    fifo_write((uint32_t)h);
    for (int i = 0; i < w * h; i++)
        fifo_write(argb[i]);       /* premultiplied BGRA, one dword/pixel */
    svga_sync();
    reg_write(SVGA_REG_CURSOR_ID, 0);
    return 0;
}

static void svga_cursor_move(int x, int y)
{
    if (!s_up) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (s_cursor_bypass) {
        /* Bypass protocol: set ON/X/Y, then bump CURSOR_COUNT so the device
         * latches the new position (mirrors vmwgfx's update sequence). */
        s_fifo[SVGA_FIFO_CURSOR_ON]    = SVGA_CURSOR_ON_SHOW;
        s_fifo[SVGA_FIFO_CURSOR_X]     = (uint32_t)x;
        s_fifo[SVGA_FIFO_CURSOR_Y]     = (uint32_t)y;
        s_fifo[SVGA_FIFO_CURSOR_COUNT] = s_fifo[SVGA_FIFO_CURSOR_COUNT] + 1u;
        return;
    }
    reg_write(SVGA_REG_CURSOR_X, (uint32_t)x);
    reg_write(SVGA_REG_CURSOR_Y, (uint32_t)y);
}

static void svga_cursor_show(int on)
{
    if (!s_up) return;
    if (s_cursor_bypass) {
        s_fifo[SVGA_FIFO_CURSOR_ON]    = on ? SVGA_CURSOR_ON_SHOW : SVGA_CURSOR_ON_HIDE;
        s_fifo[SVGA_FIFO_CURSOR_COUNT] = s_fifo[SVGA_FIFO_CURSOR_COUNT] + 1u;
        return;
    }
    reg_write(SVGA_REG_CURSOR_ON, on ? SVGA_CURSOR_ON_SHOW : SVGA_CURSOR_ON_HIDE);
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
    .present_rect  = svga_present_rect,
    .flush_rect    = svga_flush_rect,
    .cursor_define = svga_cursor_define,
    .cursor_move   = svga_cursor_move,
    .cursor_show   = svga_cursor_show,
    .caps          = VID_CAP_HW_CURSOR,
};
