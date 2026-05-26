/*
 * shell_cmd_display.c -- display-related shell commands.
 *
 * Commands: clear  fgcol  bgcol  setmode
 */

#include "shell_priv.h"

#include <kernel/tty.h>
#include <kernel/vesa.h>
#include <kernel/vesa_tty.h>
#include <kernel/vtty.h>
#include <kernel/bochs_vbe.h>
#include <kernel/admin.h>

/* ---------------------------------------------------------------------------
 * Colour palette – 16 standard CGA/VGA colours with matching VESA RGB values
 * and VGA attribute nibbles.
 * --------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    uint8_t     vga;   /* VGA colour index (0-15) */
    uint32_t    rgb;   /* 24-bit VESA RGB          */
} colour_entry_t;

static const colour_entry_t colour_palette[] = {
    { "black",         0,  0x000000 },
    { "blue",          1,  0x0000AA },
    { "green",         2,  0x00AA00 },
    { "cyan",          3,  0x00AAAA },
    { "red",           4,  0xAA0000 },
    { "magenta",       5,  0xAA00AA },
    { "brown",         6,  0xAA5500 },
    { "lightgray",     7,  0xAAAAAA },
    { "darkgray",      8,  0x555555 },
    { "lightblue",     9,  0x5555FF },
    { "lightgreen",   10,  0x55FF55 },
    { "lightcyan",    11,  0x55FFFF },
    { "lightred",     12,  0xFF5555 },
    { "lightmagenta", 13,  0xFF55FF },
    { "yellow",       14,  0xFFFF55 },
    { "white",        15,  0xFFFFFF },
};
#define PALETTE_SIZE ((int)(sizeof(colour_palette) / sizeof(colour_palette[0])))

static const colour_entry_t *palette_lookup(const char *name)
{
    for (int i = 0; i < PALETTE_SIZE; i++)
        if (strcmp(colour_palette[i].name, name) == 0)
            return &colour_palette[i];
    return NULL;
}

/* Current live colours (start at shell defaults). */
static uint8_t  s_vga_fg  = 15;   /* white */
static uint8_t  s_vga_bg  = 1;    /* blue  */
static uint32_t s_rgb_fg  = 0xFFFFFF;
static uint32_t s_rgb_bg  = 0x0000AA;

static void apply_colours(void)
{
    uint8_t attr = (uint8_t)((s_vga_bg << 4) | (s_vga_fg & 0x0F));
    t_setcolor(attr);
    if (vesa_tty_is_ready())
        vesa_tty_setcolor(s_rgb_fg, s_rgb_bg);
}

static void print_palette(void)
{
    t_writestring("colours: ");
    for (int i = 0; i < PALETTE_SIZE; i++) {
        if (i) t_writestring(", ");
        t_writestring(colour_palette[i].name);
    }
    t_putchar('\n');
}

/* ---------------------------------------------------------------------------
 * Command handlers
 * --------------------------------------------------------------------------- */

void shell_apply_scheme_for_tty(int tty)
{
    if (tty < 0 || tty >= 4) tty = 0;
    const shell_scheme_t *s = &SHELL_SCHEMES[tty];
    terminal_set_colorscheme(s->vga);
    if (vesa_tty_is_ready())
        vesa_tty_setcolor(s->fg_rgb, s->bg_rgb);
}

void shell_clear_screen(void)
{
    int tty = 0;
    task_t *t = task_current();
    if (t && t->tty >= 0 && t->tty < 4) tty = t->tty;
    shell_apply_scheme_for_tty(tty);
    if (vesa_tty_is_ready())
        vesa_tty_clear();
}

static void cmd_clear(int argc, char **argv)
{
    (void)argc; (void)argv;
    shell_clear_screen();
}

/* Privileged colour-set helpers.  Called by both the in-kernel rescue
 * shell's cmd_* wrappers and SYS_FGCOL/SYS_BGCOL from the userspace
 * shell.  Empty/NULL string prints the palette and returns -1 so the
 * caller can show usage; unknown colour returns -2 (also prints).  */
int admin_fgcol(const char *colour)
{
    if (!colour || !*colour) {
        t_writestring("usage: fgcol <colour>\n");
        print_palette();
        return -1;
    }
    const colour_entry_t *e = palette_lookup(colour);
    if (!e) {
        t_writestring("unknown colour: ");
        t_writestring(colour);
        t_putchar('\n');
        print_palette();
        return -2;
    }
    s_vga_fg = e->vga;
    s_rgb_fg = e->rgb;
    apply_colours();
    return 0;
}

int admin_bgcol(const char *colour)
{
    if (!colour || !*colour) {
        t_writestring("usage: bgcol <colour>\n");
        print_palette();
        return -1;
    }
    const colour_entry_t *e = palette_lookup(colour);
    if (!e) {
        t_writestring("unknown colour: ");
        t_writestring(colour);
        t_putchar('\n');
        print_palette();
        return -2;
    }
    s_vga_bg = e->vga;
    s_rgb_bg = e->rgb;
    apply_colours();
    if (vesa_tty_is_ready())
        vesa_tty_clear();
    return 0;
}

static void cmd_fgcol(int argc, char **argv)
{
    admin_fgcol(argc >= 2 ? argv[1] : NULL);
}

static void cmd_bgcol(int argc, char **argv)
{
    admin_bgcol(argc >= 2 ? argv[1] : NULL);
}

/* ---------------------------------------------------------------------------
 * setmode - switch display mode on the fly.
 *
 * VGA text modes (Bochs VBE disabled, QEMU reverts to mode 3):
 *   80x25  /  text   - 80×25 VGA text (9×16 px glyphs)
 *   80x50            - 80×50 VGA text (9×8 px glyphs)
 *
 * VESA framebuffer modes (set via Bochs VBE I/O ports):
 *   320x240          - 320×240×32
 *   640x480  / 480p  - 640×480×32
 *   1280x720 / 720p  - 1280×720×32
 *   1920x1080/ 1080p - 1920×1080×32
 * --------------------------------------------------------------------------- */

typedef struct { const char *name; uint32_t w; uint32_t h; } vesa_mode_t;

static const vesa_mode_t vesa_modes[] = {
    { "320x240",   320,  240  },
    { "640x480",   640,  480  },
    { "480p",      640,  480  },
    { "1280x720",  1280, 720  },
    { "720p",      1280, 720  },
    { "1920x1080", 1920, 1080 },
    { "1080p",     1920, 1080 },
};
#define VESA_MODE_COUNT ((uint32_t)(sizeof(vesa_modes) / sizeof(vesa_modes[0])))

/* Privileged setmode: NULL/empty `mode` reports the current mode and
 * returns -1; unrecognised mode returns -2 (after printing usage).
 * Returns 0 on a successful switch. */
int admin_setmode(const char *mode)
{
    if (!mode || !*mode) {
        /* No arg: report the current mode. */
        t_writestring("Mode: ");
        if (vesa_tty_is_ready()) {
            const vesa_fb_t *fb = vesa_get_fb();
            if (fb) {
                t_dec(fb->width);  t_writestring("x");
                t_dec(fb->height); t_writestring("x");
                t_dec(fb->bpp);    t_writestring(" (VESA, ");
                t_dec(vesa_tty_get_cols()); t_writestring("x");
                t_dec(vesa_tty_get_rows()); t_writestring(" cells)\n");
            } else {
                t_writestring("VESA (geometry unknown)\n");
            }
        } else {
            t_writestring("VGA text 80x");
            t_dec((uint32_t)t_get_rows());
            t_writestring("\n");
        }
        t_writestring("Usage: setmode <80x25|80x50|320x240|640x480|480p|720p|1080p>\n");
        return -1;
    }

    /* --- VGA text modes -------------------------------------------------- */
    if (strcmp(mode, "80x25") == 0 || strcmp(mode, "text") == 0 ||
        strcmp(mode, "25")    == 0) {
        bochs_vbe_disable();
        vesa_disable();
        vesa_tty_disable();
        terminal_set_rows(25);
        terminal_set_colorscheme((uint8_t)((s_vga_bg << 4) | (s_vga_fg & 0x0F)));
        t_writestring("Mode: VGA 80x25 text\n");
        return 0;
    }

    if (strcmp(mode, "80x50") == 0 || strcmp(mode, "50") == 0) {
        bochs_vbe_disable();
        vesa_disable();
        vesa_tty_disable();
        terminal_set_rows(50);
        vga_load_text_font_8x8();
        terminal_set_colorscheme((uint8_t)((s_vga_bg << 4) | (s_vga_fg & 0x0F)));
        t_writestring("Mode: VGA 80x50 text\n");
        return 0;
    }

    /* --- VESA framebuffer modes ------------------------------------------ */
    if (!bochs_vbe_available()) {
        t_writestring("Error: Bochs VBE not available on this hardware.\n");
        return -2;
    }

    for (uint32_t i = 0; i < VESA_MODE_COUNT; i++) {
        if (strcmp(mode, vesa_modes[i].name) != 0)
            continue;

        uint32_t w = vesa_modes[i].w;
        uint32_t h = vesa_modes[i].h;

        vesa_tty_set_scale(w >= 1280 ? 2 : 1);
        bochs_vbe_set_mode(w, h, 32);
        vesa_update_geometry(w, h, 32);
        vesa_tty_init();
        vtty_init();
        vesa_tty_setcolor(SHELL_FG_RGB, SHELL_BG_RGB);
        vesa_tty_clear();

        t_writestring("Mode: ");
        t_dec(w); t_writestring("x"); t_dec(h); t_writestring("x32\n");
        return 0;
    }

    t_writestring("Error: unknown mode '");
    t_writestring(mode);
    t_writestring("'\nUsage: setmode <80x25|80x50|320x240|640x480|480p|720p|1080p>\n");
    return -2;
}

static void cmd_setmode(int argc, char **argv)
{
    admin_setmode(argc >= 2 ? argv[1] : NULL);
}

/* ---------------------------------------------------------------------------
 * Module table
 * --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * caret <under|block|flash> - switch the visible-cursor style on the VESA
 * framebuffer.  No-op in pure VGA text mode (the hardware cursor is already
 * a flashing underscore there).
 * --------------------------------------------------------------------------- */
static void cmd_caret(int argc, char **argv)
{
    if (!vesa_tty_is_ready()) {
        t_writestring("caret: VESA not active - VGA hardware cursor is in use.\n");
        return;
    }
    if (argc < 2) {
        const uint32_t s = vesa_tty_get_caret_style();
        t_writestring("caret: ");
        t_writestring(s == VESA_CARET_BLOCK ? "block"
                    : s == VESA_CARET_FLASH ? "flash"
                    :                         "under");
        t_writestring("\nusage: caret <under|block|flash>\n");
        return;
    }
    const char *m = argv[1];
    if      (strcmp(m, "under") == 0) vesa_tty_set_caret_style(VESA_CARET_UNDERSCORE);
    else if (strcmp(m, "block") == 0) vesa_tty_set_caret_style(VESA_CARET_BLOCK);
    else if (strcmp(m, "flash") == 0) vesa_tty_set_caret_style(VESA_CARET_FLASH);
    else {
        t_writestring("caret: unknown style '");
        t_writestring(m);
        t_writestring("'\nusage: caret <under|block|flash>\n");
    }
}

const shell_cmd_entry_t display_cmds[] = {
    { "clear",   cmd_clear   },
    { "fgcol",   cmd_fgcol   },
    { "bgcol",   cmd_bgcol   },
    { "setmode", cmd_setmode },
    { "caret",   cmd_caret   },
    { NULL, NULL }
};
