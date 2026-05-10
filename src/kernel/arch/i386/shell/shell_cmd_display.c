/*
 * shell_cmd_display.c -- display-related shell commands.
 *
 * Commands: clear  fgcol  bgcol  setmode
 */

#include "shell_priv.h"

#include <kernel/tty.h>
#include <kernel/vesa.h>
#include <kernel/vesa_tty.h>
#include <kernel/bochs_vbe.h>

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

static void cmd_clear(int argc, char **argv)
{
    (void)argc; (void)argv;
    terminal_set_colorscheme(SHELL_COLOR_VGA);
    if (vesa_tty_is_ready()) {
        vesa_tty_setcolor(SHELL_FG_RGB, SHELL_BG_RGB);
        vesa_tty_clear();
    }
}

static void cmd_fgcol(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("usage: fgcol <colour>\n");
        print_palette();
        return;
    }
    const colour_entry_t *e = palette_lookup(argv[1]);
    if (!e) {
        t_writestring("unknown colour: ");
        t_writestring(argv[1]);
        t_putchar('\n');
        print_palette();
        return;
    }
    s_vga_fg = e->vga;
    s_rgb_fg = e->rgb;
    apply_colours();
}

static void cmd_bgcol(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("usage: bgcol <colour>\n");
        print_palette();
        return;
    }
    const colour_entry_t *e = palette_lookup(argv[1]);
    if (!e) {
        t_writestring("unknown colour: ");
        t_writestring(argv[1]);
        t_putchar('\n');
        print_palette();
        return;
    }
    s_vga_bg = e->vga;
    s_rgb_bg = e->rgb;
    apply_colours();
    if (vesa_tty_is_ready())
        vesa_tty_clear();
}

/* ---------------------------------------------------------------------------
 * setmode — switch display mode on the fly.
 *
 * VGA text modes (Bochs VBE disabled, QEMU reverts to mode 3):
 *   80x25  /  text   — 80×25 VGA text (9×16 px glyphs)
 *   80x50            — 80×50 VGA text (9×8 px glyphs)
 *
 * VESA framebuffer modes (set via Bochs VBE I/O ports):
 *   320x240          — 320×240×32
 *   640x480  / 480p  — 640×480×32
 *   1280x720 / 720p  — 1280×720×32
 *   1920x1080/ 1080p — 1920×1080×32
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

static void cmd_setmode(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("Usage: setmode <mode>\n");
        t_writestring("  Text : 80x25  80x50\n");
        t_writestring("  VESA : 320x240  640x480  480p  720p  1080p\n");
        return;
    }

    const char *mode = argv[1];

    /* --- VGA text modes -------------------------------------------------- */
    if (strcmp(mode, "80x25") == 0 || strcmp(mode, "text") == 0 ||
        strcmp(mode, "25")    == 0) {
        bochs_vbe_disable();
        vesa_disable();
        vesa_tty_disable();
        terminal_set_rows(25);
        terminal_set_colorscheme((uint8_t)((s_vga_bg << 4) | (s_vga_fg & 0x0F)));
        t_writestring("Mode: VGA 80x25 text\n");
        return;
    }

    if (strcmp(mode, "80x50") == 0 || strcmp(mode, "50") == 0) {
        bochs_vbe_disable();
        vesa_disable();
        vesa_tty_disable();
        terminal_set_rows(50);
        terminal_set_colorscheme((uint8_t)((s_vga_bg << 4) | (s_vga_fg & 0x0F)));
        t_writestring("Mode: VGA 80x50 text\n");
        return;
    }

    /* --- VESA framebuffer modes ------------------------------------------ */
    if (!bochs_vbe_available()) {
        t_writestring("Error: Bochs VBE not available on this hardware.\n");
        return;
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
        vesa_tty_setcolor(SHELL_FG_RGB, SHELL_BG_RGB);
        vesa_tty_clear();

        t_writestring("Mode: ");
        t_dec(w); t_writestring("x"); t_dec(h); t_writestring("x32\n");
        return;
    }

    t_writestring("Error: unknown mode '");
    t_writestring(mode);
    t_writestring("'\nUsage: setmode <80x25|80x50|320x240|640x480|480p|720p|1080p>\n");
}

/* ---------------------------------------------------------------------------
 * Module table
 * --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * caret <under|block|flash> — switch the visible-cursor style on the VESA
 * framebuffer.  No-op in pure VGA text mode (the hardware cursor is already
 * a flashing underscore there).
 * --------------------------------------------------------------------------- */
static void cmd_caret(int argc, char **argv)
{
    if (!vesa_tty_is_ready()) {
        t_writestring("caret: VESA not active — VGA hardware cursor is in use.\n");
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
