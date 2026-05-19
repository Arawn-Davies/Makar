/*
 * lines.elf - C64/BASIC-style string-art easter egg.
 *
 * Two endpoints drift around the screen bouncing off the edges; every
 * frame we draw the line between them in a fresh colour.  Because the
 * endpoint velocities are incommensurate the result is the classic
 * spirograph / moiré pattern from a 1980s BASIC type-in.
 *
 * Two render modes:
 *
 *   VESA (preferred) - SYS_FB_INFO reports a pixel framebuffer, so we
 *   draw with SYS_DRAW_LINE at native resolution.  RGB is randomised
 *   per line so the colour space is huge (no 16-colour limit).
 *
 *   VGA / cell-mode fallback - no pixel FB, so we Bresenham over
 *   character cells with the CP437 full-block glyph in cycling VGA
 *   colours.  Chunky but at least visible on hardware without VBE.
 *
 * Quits on 'q', 'Q', F10, or Ctrl+C.  Non-blocking stdin so the line
 * pump never pauses for input -- same idiom maktop / clock use.  The
 * shell's post-fullscreen restore path repaints the focused VT's
 * backing grid and reapplies the per-VT palette on exit, so we don't
 * reach into terminal state ourselves.
 */

#include "syscall.h"

#define MAX_COLS 160
#define MAX_ROWS 60

/* Bright VGA colours used in cell-mode only.  In pixel mode we
 * generate full 24-bit RGB instead. */
static const unsigned char PALETTE_VGA[] = {
    9, 10, 11, 12, 13, 14, 15,
};
#define PALETTE_VGA_N (sizeof(PALETTE_VGA) / sizeof(PALETTE_VGA[0]))

/* ---- shared state ---------------------------------------------------------- */

static unsigned int s_rng;
static unsigned int rng_next(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return (s_rng >> 8) & 0x7FFFFFu;
}

static int s_abs(int x) { return x < 0 ? -x : x; }

static int read_key_nb(void)
{
    unsigned char c;
    long r = sys_read(0, &c, 1);
    if (r == 1) return (int)c;
    return -1;
}

/* Pick a non-zero velocity in [-v_max..-1] U [1..v_max].  Larger speeds
 * spread the moiré bands further apart per frame. */
static int pick_velocity(int v_max)
{
    int v = (int)(rng_next() % (unsigned int)v_max) + 1;
    if (rng_next() & 1) v = -v;
    return v;
}

/* Vivid colour: avoid muddy mid-greys by forcing at least one channel
 * to be near-max.  Keeps the lines saturated against a black background. */
static unsigned int pick_rgb(void)
{
    unsigned int r = rng_next() & 0xFFu;
    unsigned int g = rng_next() & 0xFFu;
    unsigned int b = rng_next() & 0xFFu;
    unsigned int which = rng_next() % 3u;
    if (which == 0) r |= 0xC0u;
    else if (which == 1) g |= 0xC0u;
    else                 b |= 0xC0u;
    return (r << 16) | (g << 8) | b;
}

/* ---- pixel-mode renderer (VESA) ------------------------------------------- */

static int  s_px_w, s_px_h;
static int  s_px0, s_py0, s_px1, s_py1;
static int  s_pvx0, s_pvy0, s_pvx1, s_pvy1;

static void px_reseed(void)
{
    s_px0 = (int)(rng_next() % (unsigned int)s_px_w);
    s_py0 = (int)(rng_next() % (unsigned int)s_px_h);
    s_px1 = (int)(rng_next() % (unsigned int)s_px_w);
    s_py1 = (int)(rng_next() % (unsigned int)s_px_h);
    s_pvx0 = pick_velocity(7);
    s_pvy0 = pick_velocity(5);
    s_pvx1 = pick_velocity(5);
    s_pvy1 = pick_velocity(7);
}

static void px_step(void)
{
    unsigned int rgb = pick_rgb();
    sys_draw_line(s_px0, s_py0, s_px1, s_py1, rgb);

    s_px0 += s_pvx0;
    if      (s_px0 <= 0)            { s_px0 = 0;             s_pvx0 = -s_pvx0; }
    else if (s_px0 >= s_px_w - 1)   { s_px0 = s_px_w - 1;    s_pvx0 = -s_pvx0; }
    s_py0 += s_pvy0;
    if      (s_py0 <= 0)            { s_py0 = 0;             s_pvy0 = -s_pvy0; }
    else if (s_py0 >= s_px_h - 1)   { s_py0 = s_px_h - 1;    s_pvy0 = -s_pvy0; }
    s_px1 += s_pvx1;
    if      (s_px1 <= 0)            { s_px1 = 0;             s_pvx1 = -s_pvx1; }
    else if (s_px1 >= s_px_w - 1)   { s_px1 = s_px_w - 1;    s_pvx1 = -s_pvx1; }
    s_py1 += s_pvy1;
    if      (s_py1 <= 0)            { s_py1 = 0;             s_pvy1 = -s_pvy1; }
    else if (s_py1 >= s_px_h - 1)   { s_py1 = s_px_h - 1;    s_pvy1 = -s_pvy1; }
}

/* Wipe the framebuffer by drawing a fan of black lines covering the
 * canvas.  Simpler than adding a SYS_FB_CLEAR; works because the
 * drawable height already excludes the status row. */
static void px_wipe(void)
{
    for (int y = 0; y < s_px_h; y += 2)
        sys_draw_line(0, y, s_px_w - 1, y, 0x000000);
}

static int run_pixel_mode(void)
{
    px_reseed();
    px_wipe();

    unsigned int frame = 0;
    unsigned int next  = sys_uptime();

    for (;;) {
        int c = read_key_nb();
        if (c >= 0) {
            if (c == 'q' || c == 'Q' || c == 0x03 || c == (int)KEY_F10) break;
            /* On VT-switch back to us, repaint by wiping + reseeding;
             * the focused-VT logic in the kernel already handed the FB
             * back to us. */
            if (c == (int)KEY_FOCUS_GAIN) {
                px_wipe();
                px_reseed();
            }
            continue;
        }
        unsigned int now = sys_uptime();
        if ((int)(now - next) >= 0) {
            px_step();
            frame++;
            /* Periodic wipe + velocity re-roll so the pattern keeps
             * evolving rather than saturating into a solid colour. */
            if (frame % 400u == 0u) {
                px_wipe();
                s_pvx0 = pick_velocity(7); s_pvy0 = pick_velocity(5);
                s_pvx1 = pick_velocity(5); s_pvy1 = pick_velocity(7);
            }
            next = now + 1;   /* 100 Hz / 1 = 100 lines/sec */
        }
        sys_yield();
    }
    return 0;
}

/* ---- cell-mode renderer (VGA fallback) ------------------------------------ */

static int s_cols, s_rows;

static tty_cell_t s_batch[MAX_COLS + MAX_ROWS];
static int        s_nbatch;

static void cell_flush(void)
{
    if (s_nbatch > 0) {
        sys_putch_at(s_batch, (unsigned int)s_nbatch);
        s_nbatch = 0;
    }
}

static void cell_plot(int col, int row, unsigned char clr)
{
    if (col < 0 || row < 0 || col >= s_cols || row >= s_rows - 1) return;
    s_batch[s_nbatch].col = (unsigned char)col;
    s_batch[s_nbatch].row = (unsigned char)row;
    s_batch[s_nbatch].ch  = 0xDB;
    s_batch[s_nbatch].clr = clr;
    s_nbatch++;
    if (s_nbatch == (int)(sizeof(s_batch)/sizeof(s_batch[0])))
        cell_flush();
}

static void cell_line(int x0, int y0, int x1, int y1, unsigned char clr)
{
    int dx =  s_abs(x1 - x0);
    int dy = -s_abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        cell_plot(x0, y0, VGA_CLR(clr, VGA_BLACK));
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    cell_flush();
}

static int run_cell_mode(void)
{
    s_cols = (int)sys_term_cols();
    s_rows = (int)sys_term_rows();
    if (s_cols > MAX_COLS) s_cols = MAX_COLS;
    if (s_rows > MAX_ROWS) s_rows = MAX_ROWS;
    if (s_cols < 8 || s_rows < 4) return 1;

    sys_tty_clear(VGA_CLR(7, VGA_BLACK));

    int x0 = (int)(rng_next() % (unsigned int)s_cols);
    int y0 = (int)(rng_next() % (unsigned int)(s_rows - 1));
    int x1 = (int)(rng_next() % (unsigned int)s_cols);
    int y1 = (int)(rng_next() % (unsigned int)(s_rows - 1));
    int vx0 = pick_velocity(3), vy0 = pick_velocity(2);
    int vx1 = pick_velocity(2), vy1 = pick_velocity(3);

    unsigned int color_idx = 0;
    unsigned int frame     = 0;
    unsigned int next      = sys_uptime();

    for (;;) {
        int c = read_key_nb();
        if (c >= 0) {
            if (c == 'q' || c == 'Q' || c == 0x03 || c == (int)KEY_F10) break;
            if (c == (int)KEY_FOCUS_GAIN) sys_tty_clear(VGA_CLR(7, VGA_BLACK));
            continue;
        }
        unsigned int now = sys_uptime();
        if ((int)(now - next) >= 0) {
            unsigned char clr = PALETTE_VGA[color_idx];
            color_idx = (color_idx + 1) % PALETTE_VGA_N;
            cell_line(x0, y0, x1, y1, clr);

            x0 += vx0;
            if      (x0 <= 0)            { x0 = 0;             vx0 = -vx0; }
            else if (x0 >= s_cols - 1)   { x0 = s_cols - 1;    vx0 = -vx0; }
            y0 += vy0;
            if      (y0 <= 0)            { y0 = 0;             vy0 = -vy0; }
            else if (y0 >= s_rows - 2)   { y0 = s_rows - 2;    vy0 = -vy0; }
            x1 += vx1;
            if      (x1 <= 0)            { x1 = 0;             vx1 = -vx1; }
            else if (x1 >= s_cols - 1)   { x1 = s_cols - 1;    vx1 = -vx1; }
            y1 += vy1;
            if      (y1 <= 0)            { y1 = 0;             vy1 = -vy1; }
            else if (y1 >= s_rows - 2)   { y1 = s_rows - 2;    vy1 = -vy1; }

            frame++;
            if (frame % 240u == 0u) {
                sys_tty_clear(VGA_CLR(7, VGA_BLACK));
                vx0 = pick_velocity(3); vy0 = pick_velocity(2);
                vx1 = pick_velocity(2); vy1 = pick_velocity(3);
            }
            next = now + 2;
        }
        sys_yield();
    }
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    if (sys_fcntl(0, F_SETFL, O_NONBLOCK) != 0) return 1;
    s_rng = sys_uptime() * 2654435761u + 1u;

    unsigned int info = sys_fb_info();
    if (info != 0u) {
        s_px_w = (int)((info >> 16) & 0xFFFFu);
        s_px_h = (int)( info        & 0xFFFFu);
        /* Reserve one character row at the bottom for the status bar;
         * the kernel-side draw clip uses the same carve-out so this
         * is belt-and-braces but keeps endpoint motion in-bounds. */
        if (s_px_h > 24) s_px_h -= 24;
        run_pixel_mode();
    } else {
        run_cell_mode();
    }

    sys_fcntl(0, F_SETFL, 0);
    return 0;
}
