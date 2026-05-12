#include <kernel/bochs_vbe.h>
#include <kernel/asm.h>
#include <kernel/vga_8x16_font.h>
#include <kernel/vesa_font.h>

#define VBE_PORT_INDEX  0x01CEu
#define VBE_PORT_DATA   0x01CFu

#define VBE_IDX_ID      0x0u
#define VBE_IDX_XRES    0x1u
#define VBE_IDX_YRES    0x2u
#define VBE_IDX_BPP     0x3u
#define VBE_IDX_ENABLE  0x4u

#define VBE_DISABLED    0x0000u
#define VBE_ENABLED     0x0001u
#define VBE_LFB         0x0040u

#define VBE_ID_MIN  0xB0C0u
#define VBE_ID_MAX  0xB0CFu

/* VGA attribute controller ports used when re-enabling text-mode display. */
#define VGA_PORT_ISR1   0x3DAu   /* input status register 1 (resets AR flip-flop) */
#define VGA_PORT_AR     0x3C0u   /* attribute controller address/data register */
#define VGA_AR_PAS      0x20u    /* Palette Address Source - bit 5 enables video */

static uint16_t vbe_read(uint16_t idx)
{
    outw(VBE_PORT_INDEX, idx);
    return inw(VBE_PORT_DATA);
}

static void vbe_write(uint16_t idx, uint16_t val)
{
    outw(VBE_PORT_INDEX, idx);
    outw(VBE_PORT_DATA, val);
}

bool bochs_vbe_available(void)
{
    uint16_t id = vbe_read(VBE_IDX_ID);
    return (id >= VBE_ID_MIN && id <= VBE_ID_MAX);
}

void bochs_vbe_set_mode(uint32_t width, uint32_t height, uint8_t bpp)
{
    vbe_write(VBE_IDX_ENABLE, VBE_DISABLED);
    vbe_write(VBE_IDX_XRES,   (uint16_t)width);
    vbe_write(VBE_IDX_YRES,   (uint16_t)height);
    vbe_write(VBE_IDX_BPP,    (uint16_t)bpp);
    vbe_write(VBE_IDX_ENABLE, VBE_ENABLED | VBE_LFB);
}

/* Standard CGA 16-colour palette, 6-bit per channel, in the order the VGA
 * DAC expects (palette indices 0..15 are what text-mode attribute bytes
 * pick from).  After VBE switched the DAC to a linear graphics-mode
 * palette, attribute-byte writes don't render visibly any more - every
 * cell shows up as black.  Restoring these 16 entries gets text mode
 * looking correct again.
 *
 * Values match IBM's original CGA palette as documented in the OSDev
 * VGA Hardware article (https://wiki.osdev.org/VGA_Hardware). */
static const uint8_t cga_dac_palette[16][3] = {
    { 0,  0,  0  },  /*  0 BLACK     */
    { 0,  0,  42 },  /*  1 BLUE      */
    { 0,  42, 0  },  /*  2 GREEN     */
    { 0,  42, 42 },  /*  3 CYAN      */
    { 42, 0,  0  },  /*  4 RED       */
    { 42, 0,  42 },  /*  5 MAGENTA   */
    { 42, 21, 0  },  /*  6 BROWN     */
    { 42, 42, 42 },  /*  7 LGREY     */
    { 21, 21, 21 },  /*  8 DGREY     */
    { 21, 21, 63 },  /*  9 LBLUE     */
    { 21, 63, 21 },  /* 10 LGREEN    */
    { 21, 63, 63 },  /* 11 LCYAN     */
    { 63, 21, 21 },  /* 12 LRED      */
    { 63, 21, 63 },  /* 13 LMAGENTA  */
    { 63, 63, 21 },  /* 14 YELLOW    */
    { 63, 63, 63 },  /* 15 WHITE     */
};

/* IBM VGA Mode 03h (80×25 colour text) register dump.  Cross-checked
 * against the OSDev VGA Hardware article and the FreeDOS mode_03 table. */
static const uint8_t mode3_seq[5] = {
    0x03, 0x00, 0x03, 0x00, 0x02
};
static const uint8_t mode3_crtc[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50,
    0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF
};
static const uint8_t mode3_gc[9] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF
};
static const uint8_t mode3_ac[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x0C, 0x00, 0x0F, 0x08, 0x00
};

static void vga_load_mode_3(void)
{
    outb(0x3C2, 0x67);                          /* Misc Output       */

    for (uint8_t i = 0; i < 5; i++) {           /* Sequencer         */
        outb(0x3C4, i);
        outb(0x3C5, mode3_seq[i]);
    }

    outb(0x3D4, 0x11);                          /* unlock CRTC 0..7  */
    outb(0x3D5, (uint8_t)(inb(0x3D5) & 0x7F));
    for (uint8_t i = 0; i < 25; i++) {          /* CRTC              */
        outb(0x3D4, i);
        outb(0x3D5, mode3_crtc[i]);
    }

    for (uint8_t i = 0; i < 9; i++) {           /* Graphics Ctrl     */
        outb(0x3CE, i);
        outb(0x3CF, mode3_gc[i]);
    }

    inb(VGA_PORT_ISR1);                         /* AC flip-flop      */
    for (uint8_t i = 0; i < 21; i++) {          /* Attribute Ctrl    */
        outb(VGA_PORT_AR, i);
        outb(VGA_PORT_AR, mode3_ac[i]);
    }
    outb(VGA_PORT_AR, VGA_AR_PAS);              /* re-enable display */

    outb(0x3C8, 0);                             /* DAC palette       */
    for (int i = 0; i < 16; i++) {
        outb(0x3C9, cga_dac_palette[i][0]);
        outb(0x3C9, cga_dac_palette[i][1]);
        outb(0x3C9, cga_dac_palette[i][2]);
    }
}

/* Reload plane-2 font data after a VESA session.  VBE leaves plane 2
 * full of graphics pixels, which renders as junk glyphs in text mode.
 * Uses the real 8×16 IBM VGA ROM font (FONT8x16) so 80×25 cells get
 * proper full-height glyphs and 80×50 cells get the top 8 scanlines
 * (the hardware reads only the first 8 bytes of each 32-byte slot
 * when CRTC max-scan-line is 7).
 *
 * Follows Linux vgacon's recipe: bracket the sequencer/GC reprogramming
 * with a synchronous reset (Seq[0] = 1 → 3) so the chip doesn't see
 * half-changed state mid-write. */
static void vga_load_text_font(void)
{
    outb(0x3C4, 0); outb(0x3C5, 0x01);   /* sync reset start             */

    outb(0x3C4, 2); outb(0x3C5, 0x04);   /* map mask = plane 2 only      */
    outb(0x3C4, 4); outb(0x3C5, 0x07);   /* mem mode: ext + sequential   */

    outb(0x3C4, 0); outb(0x3C5, 0x03);   /* sync reset end               */

    outb(0x3CE, 4); outb(0x3CF, 0x02);   /* read map select = plane 2    */
    outb(0x3CE, 5); outb(0x3CF, 0x00);   /* graphics mode: write mode 0  */
    outb(0x3CE, 6); outb(0x3CF, 0x00);   /* misc: A0000-BFFFF, alpha     */

    volatile uint8_t *plane2 = (volatile uint8_t *)0xA0000;
    for (int c = 0; c < 256; c++) {
        volatile uint8_t *dst = plane2 + c * 32;
        for (int y = 0; y < VGA_FONT_8X16_BYTES_PER_GLYPH; y++)
            dst[y] = FONT8x16[c][y];
        for (int y = VGA_FONT_8X16_BYTES_PER_GLYPH; y < 32; y++)
            dst[y] = 0;
    }

    outb(0x3C4, 0); outb(0x3C5, 0x01);   /* sync reset start             */

    outb(0x3C4, 2); outb(0x3C5, 0x03);   /* map mask: planes 0+1 (text)  */
    outb(0x3C4, 4); outb(0x3C5, 0x02);   /* mem mode: text (odd/even on) */

    outb(0x3C4, 0); outb(0x3C5, 0x03);   /* sync reset end               */

    outb(0x3CE, 4); outb(0x3CF, 0x00);   /* read map = plane 0 (chars)   */
    outb(0x3CE, 5); outb(0x3CF, 0x10);   /* graphics mode: odd/even      */
    outb(0x3CE, 6); outb(0x3CF, 0x0E);   /* misc: B8000-BFFFF 32K text   */
}

void bochs_vbe_disable(void)
{
    vbe_write(VBE_IDX_ENABLE, VBE_DISABLED);
    vga_load_mode_3();
    vga_load_text_font();
}

/* The kernel's FONT8x8 stores glyphs LSB-first (bit 0 = leftmost pixel),
 * matching how vesa_tty.c rasterises them (`bits & (1u << x)` with
 * x=0 = leftmost screen pixel).  VGA hardware reads plane-2 font bytes
 * MSB-first (bit 7 = leftmost), so each byte needs flipping on upload
 * - otherwise every 80×50 glyph comes out horizontally mirrored.
 * FONT8x16 is already in the VGA-native MSB-first order, so its upload
 * path doesn't need this. */
static inline uint8_t bitrev8(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
    b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
    return b;
}

/* Public sibling of vga_load_text_font().  Same plane-2 protocol, but
 * sources from FONT8x8 instead of FONT8x16 - the 80×50 CRTC only reads
 * the first 8 bytes of each glyph slot, where an 8×16 source clips the
 * letter body.  Each cell renders as the native 8-row glyph plus 8
 * blank trailing rows (zeroed, harmless). */
void vga_load_text_font_8x8(void)
{
    outb(0x3C4, 0); outb(0x3C5, 0x01);   /* sync reset start             */

    outb(0x3C4, 2); outb(0x3C5, 0x04);   /* map mask = plane 2 only      */
    outb(0x3C4, 4); outb(0x3C5, 0x07);   /* mem mode: ext + sequential   */

    outb(0x3C4, 0); outb(0x3C5, 0x03);   /* sync reset end               */

    outb(0x3CE, 4); outb(0x3CF, 0x02);   /* read map select = plane 2    */
    outb(0x3CE, 5); outb(0x3CF, 0x00);   /* graphics mode: write mode 0  */
    outb(0x3CE, 6); outb(0x3CF, 0x00);   /* misc: A0000-BFFFF, alpha     */

    volatile uint8_t *plane2 = (volatile uint8_t *)0xA0000;
    for (int c = 0; c < 256; c++) {
        volatile uint8_t *dst = plane2 + c * 32;
        if (c < 128) {
            const uint8_t *src = FONT8x8[c];
            for (int y = 0; y < FONT8x8_CHAR_H; y++)
                dst[y] = bitrev8(src[y]);
        } else {
            for (int y = 0; y < FONT8x8_CHAR_H; y++) dst[y] = 0;
        }
        for (int y = FONT8x8_CHAR_H; y < 32; y++) dst[y] = 0;
    }

    outb(0x3C4, 0); outb(0x3C5, 0x01);   /* sync reset start             */

    outb(0x3C4, 2); outb(0x3C5, 0x03);   /* map mask: planes 0+1 (text)  */
    outb(0x3C4, 4); outb(0x3C5, 0x02);   /* mem mode: text (odd/even on) */

    outb(0x3C4, 0); outb(0x3C5, 0x03);   /* sync reset end               */

    outb(0x3CE, 4); outb(0x3CF, 0x00);   /* read map = plane 0 (chars)   */
    outb(0x3CE, 5); outb(0x3CF, 0x10);   /* graphics mode: odd/even      */
    outb(0x3CE, 6); outb(0x3CF, 0x0E);   /* misc: B8000-BFFFF 32K text   */
}
