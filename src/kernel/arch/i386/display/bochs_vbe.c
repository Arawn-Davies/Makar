#include <kernel/bochs_vbe.h>
#include <kernel/asm.h>

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
#define VGA_AR_PAS      0x20u    /* Palette Address Source — bit 5 enables video */

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
 * palette, attribute-byte writes don't render visibly any more — every
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

void bochs_vbe_disable(void)
{
    vbe_write(VBE_IDX_ENABLE, VBE_DISABLED);

    /* After disabling VBE the VGA attribute controller's PAS bit may be
     * clear, which blanks all video output.  Reset the flip-flop by reading
     * ISR1, then write the AR address register with PAS=1 to re-enable. */
    inb(VGA_PORT_ISR1);
    outb(VGA_PORT_AR, VGA_AR_PAS);

    /* Reload the CGA 16-colour DAC palette.  VESA modes leave the DAC in
     * linear graphics-palette state, which renders text-mode attribute
     * bytes invisibly — `setmode 80x25` and `setmode 80x50` showed a
     * solid black screen until the colours were restored. */
    outb(0x3C8, 0);  /* DAC write index, start at colour 0 */
    for (int i = 0; i < 16; i++) {
        outb(0x3C9, cga_dac_palette[i][0]);
        outb(0x3C9, cga_dac_palette[i][1]);
        outb(0x3C9, cga_dac_palette[i][2]);
    }
}
