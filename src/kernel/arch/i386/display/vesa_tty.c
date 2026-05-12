#include <kernel/vesa_tty.h>
#include <kernel/vesa.h>
#include <kernel/vesa_font.h>
#include <kernel/paging.h>
#include <kernel/serial.h>
#include <string.h>

/* Scale factor applied to every glyph pixel.  2 makes each 8×8 glyph appear
   as a 16×16 cell on the framebuffer, which is far more readable at typical
   VESA resolutions.  Can be changed at runtime via vesa_tty_set_scale(). */
static uint32_t font_scale = 2;

#define FONT_CELL_W (FONT8x8_CHAR_W * font_scale)
#define FONT_CELL_H (FONT8x8_CHAR_H * font_scale)

static bool         tty_ready = false;
static uint32_t     tty_cols;          /* total columns on screen */
static uint32_t     tty_rows;          /* total rows on screen    */
static vesa_pane_t  default_pane;      /* covers the whole screen */

/* Compose a framebuffer pixel value from 8-bit R, G, B components using the
   channel layout reported by the bootloader. */
static uint32_t compose_colour(uint8_t r, uint8_t g, uint8_t b)
{
	const vesa_fb_t *fb = vesa_get_fb();
	return ((uint32_t)r << fb->red_shift)   |
	       ((uint32_t)g << fb->green_shift) |
	       ((uint32_t)b << fb->blue_shift);
}

static uint32_t compose_rgb(uint32_t rgb)
{
	return compose_colour((uint8_t)((rgb >> 16) & 0xFF),
	                      (uint8_t)((rgb >>  8) & 0xFF),
	                      (uint8_t)( rgb        & 0xFF));
}

/* ------------------------------------------------------------------ */
/* Visible caret on the default pane                                    */
/*                                                                      */
/* The hardware text-mode cursor stops being visible the moment we      */
/* switch to the framebuffer, so we render our own underline strip at   */
/* the bottom of the active cell.  We stash the framebuffer pixels      */
/* underneath the strip so we can restore them when the cursor moves.   */
/* If the cell is overwritten by a character draw, the stash is         */
/* invalidated (don't restore - the new char is the truth).             */
/* ------------------------------------------------------------------ */

#define CARET_STASH_MAX  1024  /* worst case: FONT_CELL_W*FONT_CELL_H @ scale 4 */

/* Caret style.  Mirrored in vesa_tty.h as VESA_CARET_*. */
static uint32_t caret_style       = 0;     /* 0=UNDER, 1=BLOCK, 2=FLASH    */
static bool     caret_drawn       = false; /* currently painted on screen   */
static bool     caret_blink_off   = false; /* flash state (off-half cycle)  */
static uint32_t caret_abs_col     = 0;
static uint32_t caret_abs_row     = 0;
static uint32_t caret_strip_w_px  = 0;
static uint32_t caret_strip_h_px  = 0;
static uint32_t caret_stash[CARET_STASH_MAX];

/* Compute the pixel rectangle the caret currently occupies, given style. */
static void caret_rect_for(uint32_t abs_col, uint32_t abs_row,
                           uint32_t *out_px, uint32_t *out_py,
                           uint32_t *out_w,  uint32_t *out_h)
{
	uint32_t px = abs_col * FONT_CELL_W;
	uint32_t py = abs_row * FONT_CELL_H;
	uint32_t w  = FONT_CELL_W;
	uint32_t h  = (caret_style == 1) ? FONT_CELL_H : font_scale;
	if (w * h > CARET_STASH_MAX)
		h = CARET_STASH_MAX / w;
	uint32_t y0 = py + FONT_CELL_H - h;
	*out_px = px; *out_py = y0; *out_w = w; *out_h = h;
}

static void caret_save_stash(uint32_t abs_col, uint32_t abs_row)
{
	const vesa_fb_t *fb = vesa_get_fb();
	uint32_t px, py, w, h;
	caret_rect_for(abs_col, abs_row, &px, &py, &w, &h);
	caret_strip_w_px = w;
	caret_strip_h_px = h;
	for (uint32_t y = 0; y < h; y++) {
		uint32_t *src = (uint32_t *)((uint8_t *)fb->addr + (py + y) * fb->pitch);
		for (uint32_t x = 0; x < w; x++)
			caret_stash[y * w + x] = src[px + x];
	}
}

static void caret_restore_stash(uint32_t abs_col, uint32_t abs_row)
{
	const vesa_fb_t *fb = vesa_get_fb();
	uint32_t px = abs_col * FONT_CELL_W;
	uint32_t py = abs_row * FONT_CELL_H;
	uint32_t y0 = py + FONT_CELL_H - caret_strip_h_px;
	for (uint32_t y = 0; y < caret_strip_h_px; y++) {
		uint32_t *dst = (uint32_t *)((uint8_t *)fb->addr + (y0 + y) * fb->pitch);
		for (uint32_t x = 0; x < caret_strip_w_px; x++)
			dst[px + x] = caret_stash[y * caret_strip_w_px + x];
	}
}

static void caret_paint(const vesa_pane_t *p, uint32_t abs_col, uint32_t abs_row)
{
	const vesa_fb_t *fb = vesa_get_fb();
	uint32_t px, py, w, h;
	caret_rect_for(abs_col, abs_row, &px, &py, &w, &h);
	for (uint32_t y = 0; y < h; y++) {
		uint32_t *dst = (uint32_t *)((uint8_t *)fb->addr + (py + y) * fb->pitch);
		for (uint32_t x = 0; x < w; x++)
			dst[px + x] = p->fg;
	}
}

/* If a draw is about to land on the caret's cell, the stash will no
 * longer reflect what is on screen.  Drop it so a future cursor move
 * doesn't smear stale pixels back over the new content. */
static void caret_invalidate_at(uint32_t abs_col, uint32_t abs_row)
{
	if (caret_drawn &&
	    abs_col == caret_abs_col && abs_row == caret_abs_row)
		caret_drawn = false;
}

/* Render glyph for character c at absolute cell (abs_col, abs_row) using the
 * given pane's fg/bg.  Caller has already clipped to the pane. */
static void draw_char(const vesa_pane_t *p, char c,
                      uint32_t abs_col, uint32_t abs_row)
{
	caret_invalidate_at(abs_col, abs_row);

	uint8_t idx = (uint8_t)c;
	if (idx >= 128)
		idx = 0;

	const uint8_t *glyph = FONT8x8[idx];
	uint32_t px = abs_col * FONT_CELL_W;
	uint32_t py = abs_row * FONT_CELL_H;

	for (uint32_t y = 0; y < FONT8x8_CHAR_H; y++) {
		uint8_t bits = glyph[y];
		for (uint32_t x = 0; x < FONT8x8_CHAR_W; x++) {
			uint32_t colour = (bits & (1u << x)) ? p->fg : p->bg;
			for (uint32_t sy = 0; sy < font_scale; sy++)
				for (uint32_t sx = 0; sx < font_scale; sx++)
					vesa_put_pixel(px + x * font_scale + sx,
					               py + y * font_scale + sy, colour);
		}
	}
}

/* Scroll a pane up by one character row, clearing the bottom row to bg. */
static void pane_scroll_up(vesa_pane_t *p)
{
	/* Scrolling moves pixels arbitrarily; any cached caret strip is now
	 * meaningless.  Caller will re-set the cursor afterwards. */
	caret_drawn = false;
	const vesa_fb_t *fb = vesa_get_fb();
	uint8_t *base = (uint8_t *)fb->addr;

	uint32_t y_top    = p->top_row * FONT_CELL_H;
	uint32_t y_bottom = (p->top_row + p->rows) * FONT_CELL_H; /* exclusive */
	uint32_t shift    = FONT_CELL_H;

	if (p->rows > 1) {
		memmove(base + y_top * fb->pitch,
		        base + (y_top + shift) * fb->pitch,
		        (p->rows - 1) * FONT_CELL_H * fb->pitch);
	}

	/* Clear the newly vacated bottom row of the pane. */
	for (uint32_t y = y_bottom - shift; y < y_bottom; y++) {
		uint32_t *scanline = (uint32_t *)((uint8_t *)fb->addr + y * fb->pitch);
		for (uint32_t x = 0; x < fb->width; x++)
			scanline[x] = p->bg;
	}
}

/* ------------------------------------------------------------------ */
/* Init / status                                                       */
/* ------------------------------------------------------------------ */

bool vesa_tty_init(void)
{
	const vesa_fb_t *fb = vesa_get_fb();
	if (!fb)
		return false;

	/* The framebuffer lives at a high physical address outside the boot-time
	   0–8 MiB identity mapping.  Map it before writing a single pixel. */
	paging_map_region((uint32_t)(uintptr_t)fb->addr,
	                  fb->pitch * fb->height);

	tty_cols = fb->width  / FONT_CELL_W;
	tty_rows = fb->height / FONT_CELL_H;

	default_pane.top_row = 0;
	default_pane.cols    = tty_cols;
	default_pane.rows    = tty_rows;
	default_pane.cur_col = 0;
	default_pane.cur_row = 0;
	default_pane.fg = compose_colour(0xFF, 0xFF, 0xFF); /* white */
	default_pane.bg = compose_colour(0x00, 0x00, 0xAA); /* blue  */

	vesa_clear(default_pane.bg);

	tty_ready = true;
	KLOG("vesa_tty: init OK (");
	KLOG_DEC(tty_cols);
	KLOG("x");
	KLOG_DEC(tty_rows);
	KLOG(" chars, scale=");
	KLOG_DEC(font_scale);
	KLOG(")\n");
	return true;
}

bool vesa_tty_is_ready(void) { return tty_ready; }
void vesa_tty_disable(void)  { tty_ready = false; }

uint32_t vesa_tty_get_cols(void) { return tty_cols; }
uint32_t vesa_tty_get_rows(void) { return tty_rows; }

vesa_pane_t *vesa_tty_default_pane(void) { return &default_pane; }

/* ------------------------------------------------------------------ */
/* Pane API                                                            */
/* ------------------------------------------------------------------ */

void vesa_tty_pane_init(vesa_pane_t *p, uint32_t top_row, uint32_t rows)
{
	p->top_row = top_row;
	p->cols    = tty_cols;     /* zero before init - fine, gets fixed up later */
	p->rows    = rows;
	p->cur_col = 0;
	p->cur_row = 0;
	/* Default colours; caller can override before drawing. */
	if (tty_ready) {
		p->fg = compose_colour(0xFF, 0xFF, 0xFF);
		p->bg = compose_colour(0x00, 0x00, 0x00);
	} else {
		p->fg = 0;
		p->bg = 0;
	}
}

void vesa_tty_pane_setcolor(vesa_pane_t *p, uint32_t fg_rgb, uint32_t bg_rgb)
{
	p->fg = compose_rgb(fg_rgb);
	p->bg = compose_rgb(bg_rgb);
}

void vesa_tty_pane_putchar(vesa_pane_t *p, char c)
{
	if (!tty_ready)
		return;

	/* The timer ISR may preempt between a cursor increment and its bounds
	 * check, leaving cur_col/cur_row transiently out of range.  Clamp here
	 * so the draw never receives an invalid position. */
	if (p->cur_col >= p->cols && p->cols > 0) {
		p->cur_col = 0;
		if (++p->cur_row >= p->rows) {
			pane_scroll_up(p);
			p->cur_row = p->rows - 1;
		}
	}
	if (p->cur_row >= p->rows && p->rows > 0) {
		pane_scroll_up(p);
		p->cur_row = p->rows - 1;
	}

	if (c == '\n') {
		p->cur_col = 0;
		if (++p->cur_row >= p->rows) {
			pane_scroll_up(p);
			p->cur_row = p->rows - 1;
		}
		return;
	}

	if (c == '\r') {
		p->cur_col = 0;
		return;
	}

	if (c == '\b') {
		if (p->cur_col > 0)
			p->cur_col--;
		draw_char(p, ' ', p->cur_col, p->top_row + p->cur_row);
		return;
	}

	draw_char(p, c, p->cur_col, p->top_row + p->cur_row);

	if (++p->cur_col >= p->cols) {
		p->cur_col = 0;
		if (++p->cur_row >= p->rows) {
			pane_scroll_up(p);
			p->cur_row = p->rows - 1;
		}
	}
}

void vesa_tty_pane_put_at(vesa_pane_t *p, char c, uint32_t col, uint32_t row)
{
	if (!tty_ready)
		return;
	if (col >= p->cols || row >= p->rows)
		return;
	draw_char(p, c, col, p->top_row + row);
}

void vesa_tty_pane_set_cursor(vesa_pane_t *p, uint32_t col, uint32_t row)
{
	if (col < p->cols) p->cur_col = col;
	if (row < p->rows) p->cur_row = row;

	/* Visible caret only on the screen-spanning default pane (the active
	 * shell input).  Other panes (vics editor, future tmux splitters) own
	 * their own caret rendering if/when they need one. */
	if (!tty_ready || p != &default_pane)
		return;

	uint32_t new_abs_col = p->cur_col;
	uint32_t new_abs_row = p->top_row + p->cur_row;

	if (caret_drawn &&
	    (new_abs_col != caret_abs_col || new_abs_row != caret_abs_row)) {
		caret_restore_stash(caret_abs_col, caret_abs_row);
		caret_drawn = false;
	}
	if (!caret_drawn) {
		caret_save_stash(new_abs_col, new_abs_row);
		caret_abs_col = new_abs_col;
		caret_abs_row = new_abs_row;
		caret_drawn   = true;
	}
	/* In FLASH mode the caret may be in its off-half cycle; leave the
	 * stashed pixels visible so the blink updater can toggle without
	 * needing a re-stash. */
	if (!(caret_style == 2 && caret_blink_off))
		caret_paint(p, new_abs_col, new_abs_row);
}

/* Public: change caret rendering style.  0=underscore, 1=block, 2=flash. */
void vesa_tty_set_caret_style(uint32_t style)
{
	if (style > 2) style = 0;
	if (!tty_ready) { caret_style = style; return; }

	/* Erase the existing caret with the old geometry before flipping
	 * style - otherwise a UNDER->BLOCK switch would leave the underline
	 * pixels behind. */
	if (caret_drawn) {
		caret_restore_stash(caret_abs_col, caret_abs_row);
		caret_drawn = false;
	}
	caret_style     = style;
	caret_blink_off = false;
	/* Re-stash and paint at the current cursor position with new geometry. */
	caret_save_stash(default_pane.cur_col,
	                 default_pane.top_row + default_pane.cur_row);
	caret_abs_col = default_pane.cur_col;
	caret_abs_row = default_pane.top_row + default_pane.cur_row;
	caret_drawn   = true;
	caret_paint(&default_pane, caret_abs_col, caret_abs_row);
}

uint32_t vesa_tty_get_caret_style(void)
{
	return caret_style;
}

/* Periodic call (e.g. from the keyboard wait loop) that toggles the caret
 * on/off when the FLASH style is active.  ticks is the kernel tick counter
 * (100 Hz); we flip every ~250 ms ⇒ every 25 ticks. */
void vesa_tty_caret_blink_tick(uint32_t ticks)
{
	if (!tty_ready || caret_style != 2 || !caret_drawn)
		return;
	bool want_off = ((ticks / 25u) & 1u) != 0u;
	if (want_off == caret_blink_off)
		return;
	caret_blink_off = want_off;
	if (want_off)
		caret_restore_stash(caret_abs_col, caret_abs_row);
	else
		caret_paint(&default_pane, caret_abs_col, caret_abs_row);
}

void vesa_tty_pane_clear(vesa_pane_t *p)
{
	caret_drawn = false;
	p->cur_col = 0;
	p->cur_row = 0;
	if (!tty_ready)
		return;

	const vesa_fb_t *fb = vesa_get_fb();
	uint32_t y_top    = p->top_row * FONT_CELL_H;
	uint32_t y_bottom = (p->top_row + p->rows) * FONT_CELL_H;
	for (uint32_t y = y_top; y < y_bottom; y++) {
		uint32_t *scanline = (uint32_t *)((uint8_t *)fb->addr + y * fb->pitch);
		for (uint32_t x = 0; x < fb->width; x++)
			scanline[x] = p->bg;
	}
}

uint32_t vesa_tty_pane_get_col(const vesa_pane_t *p) { return p->cur_col; }
uint32_t vesa_tty_pane_get_row(const vesa_pane_t *p) { return p->cur_row; }

/* ------------------------------------------------------------------ */
/* Legacy global API - delegates to the default pane                  */
/* ------------------------------------------------------------------ */

uint32_t vesa_tty_get_col(void) { return default_pane.cur_col; }
uint32_t vesa_tty_get_row(void) { return default_pane.cur_row; }

void vesa_tty_putchar(char c)
{
	vesa_tty_pane_putchar(&default_pane, c);
}

void vesa_tty_setcolor(uint32_t fg_rgb, uint32_t bg_rgb)
{
	vesa_tty_pane_setcolor(&default_pane, fg_rgb, bg_rgb);
}

void vesa_tty_put_at(char c, uint32_t col, uint32_t row)
{
	vesa_tty_pane_put_at(&default_pane, c, col, row);
}

void vesa_tty_set_cursor(uint32_t col, uint32_t row)
{
	vesa_tty_pane_set_cursor(&default_pane, col, row);
}

void vesa_tty_clear(void)
{
	if (!tty_ready)
		return;
	/* Match legacy behaviour: clear the whole screen, including any sub-pane
	 * regions that may have been carved out by phase-3 callers. */
	vesa_clear(default_pane.bg);
	default_pane.cur_col = 0;
	default_pane.cur_row = 0;
	/* The full-framebuffer paint also wiped any caret strip we'd drawn
	 * and the pixels stashed under it; mark the cache invalid so the
	 * next set_cursor saves fresh pixels instead of restoring stale
	 * ones over the now-blank cell. */
	caret_drawn = false;
}

void vesa_tty_spinner_tick(uint32_t tick)
{
	if (!tty_ready)
		return;
	static const char frames[] = {'|', '/', '-', '\\'};
	char c = frames[(tick / 12) % 4];
	/* Always top-right of the physical screen, regardless of pane carve-up. */
	draw_char(&default_pane, c, tty_cols - 1, 0);
}

void vesa_tty_set_scale(uint32_t scale)
{
	if (scale == 0)
		scale = 1;
	font_scale = scale;   /* always update - vesa_tty_init() uses this */
}
