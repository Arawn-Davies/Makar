/* gui_ui.c -- immediate-mode widgets.  See gui_ui.h. */
#include "gui_ui.h"
#include "syscall.h"            /* KEY_ARROW_* sentinels */

static int ui_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int pt_in(ui_ctx *c, int x, int y, int w, int h)
{
    return c->mx >= x && c->mx < x + w && c->my >= y && c->my < y + h;
}

void ui_begin(ui_ctx *c, int mx, int my, int mdown, int mpressed,
              int mreleased, int key)
{
    c->mx = mx; c->my = my;
    c->mdown = mdown; c->mpressed = mpressed; c->mreleased = mreleased;
    c->key = key;
    c->cur_id = 0;
    c->got_input = 0;
    /* `active` and `focus` persist across frames (drag / kb focus). */
}

void ui_label(ui_ctx *c, gfx_surface *s, int x, int y, const char *str, gfx_u32 fg)
{
    (void)c;
    gfx_str(s, x, y, str, fg);
}

int ui_button(ui_ctx *c, gfx_surface *s, int x, int y, int w, int h,
              const char *label)
{
    int id  = ++c->cur_id;
    int hot = pt_in(c, x, y, w, h);
    int clicked = 0;

    if (hot && c->mpressed && !c->active) { c->active = id; c->got_input = 1; }
    if (c->active == id && c->mreleased) { if (hot) clicked = 1; c->active = 0; }

    gfx_u32 col = (c->active == id) ? UI_COL_BTN_ACT
                : hot              ? UI_COL_BTN_HOT
                                   : UI_COL_BTN;
    gfx_round(s, x, y, w, h, col, UI_COL_BORDER);
    int tw = gfx_text_w(label);
    gfx_str_clip(s, x + (w - tw) / 2, y + (h - 8) / 2, label, UI_COL_TEXT, x + w - 2);
    return clicked;
}

int ui_slider(ui_ctx *c, gfx_surface *s, int x, int y, int w, int h,
              int *val, int lo, int hi)
{
    int id  = ++c->cur_id;
    int hot = pt_in(c, x, y, w, h);
    int changed = 0;
    if (hi <= lo) hi = lo + 1;

    if (hot && c->mpressed && !c->active) { c->active = id; c->focus = id; c->got_input = 1; }
    if (c->active == id && c->mdown) {
        int rel = c->mx - x;
        if (rel < 0) rel = 0;
        if (rel > w) rel = w;
        int nv = lo + (rel * (hi - lo) + w / 2) / w;
        if (nv != *val) { *val = nv; changed = 1; }
    }
    if (c->active == id && c->mreleased) c->active = 0;

    if (*val < lo) *val = lo;
    if (*val > hi) *val = hi;

    int cy = y + h / 2;
    gfx_fill(s, x, cy - 2, w, 4, UI_COL_TRACK);                 /* track */
    int kx = x + (*val - lo) * w / (hi - lo);
    gfx_round(s, kx - 4, y, 8, h, UI_COL_BTN_ACT, UI_COL_BORDER); /* knob */
    return changed;
}

static int ui_textbox_impl(ui_ctx *c, gfx_surface *s, int x, int y, int w, int h,
                           char *buf, int cap, int masked)
{
    int id  = ++c->cur_id;
    int hot = pt_in(c, x, y, w, h);
    int changed = 0;

    if (hot && c->mpressed) { c->focus = id; c->got_input = 1; }

    if (c->focus == id && c->key >= 0) {
        int k = c->key, len = ui_strlen(buf);
        if (k == 8 || k == 127) {                 /* backspace */
            if (len > 0) { buf[len - 1] = 0; changed = 1; }
        } else if (k >= 32 && k < 127) {          /* printable */
            if (len < cap - 1) { buf[len] = (char)k; buf[len + 1] = 0; changed = 1; }
        }
        c->got_input = 1;
    }

    int focused = (c->focus == id);
    gfx_fill(s, x, y, w, h, focused ? UI_COL_FIELD_FC : UI_COL_FIELD);
    gfx_outline(s, x, y, w, h, focused ? UI_COL_BTN_ACT : UI_COL_BORDER);

    int tx = x + 4, ty = y + (h - 8) / 2;
    int len = ui_strlen(buf);
    int maxchars = (w - 8) / 8;
    int shown = len > maxchars ? maxchars : len;   /* tail that fits */
    if (masked) {
        for (int i = 0; i < shown; i++) gfx_char(s, tx + i * 8, ty, '*', UI_COL_TEXT);
    } else {
        const char *show = buf;
        if (len > maxchars) show = buf + (len - maxchars);
        gfx_str_clip(s, tx, ty, show, UI_COL_TEXT, x + w - 2);
    }
    if (focused)
        gfx_fill(s, tx + shown * 8, ty, 2, 8, UI_COL_TEXT);    /* caret */
    return changed;
}

int ui_textbox(ui_ctx *c, gfx_surface *s, int x, int y, int w, int h,
               char *buf, int cap)
{
    return ui_textbox_impl(c, s, x, y, w, h, buf, cap, 0);
}

int ui_password(ui_ctx *c, gfx_surface *s, int x, int y, int w, int h,
                char *buf, int cap)
{
    return ui_textbox_impl(c, s, x, y, w, h, buf, cap, 1);
}

int ui_listbox(ui_ctx *c, gfx_surface *s, int x, int y, int w, int h,
               const char *const *items, int n, int *sel, int *scroll)
{
    int id  = ++c->cur_id;
    int hot = pt_in(c, x, y, w, h);
    int changed = 0;
    const int rowh = 12;
    int vis = h / rowh; if (vis < 1) vis = 1;

    if (hot && c->mpressed) {
        c->focus = id; c->got_input = 1;
        int row = *scroll + (c->my - y) / rowh;
        if (row >= 0 && row < n && row != *sel) { *sel = row; changed = 1; }
    }
    if (c->focus == id && c->key >= 0) {
        if (c->key == KEY_ARROW_UP && *sel > 0)        { (*sel)--; changed = 1; c->got_input = 1; }
        else if (c->key == KEY_ARROW_DOWN && *sel < n - 1) { (*sel)++; changed = 1; c->got_input = 1; }
    }

    /* keep the selection visible */
    if (*sel < *scroll) *scroll = *sel;
    if (*sel >= *scroll + vis) *scroll = *sel - vis + 1;
    if (*scroll < 0) *scroll = 0;

    gfx_fill(s, x, y, w, h, UI_COL_FIELD);
    gfx_outline(s, x, y, w, h, (c->focus == id) ? UI_COL_BTN_ACT : UI_COL_BORDER);
    for (int r = 0; r < vis; r++) {
        int idx = *scroll + r;
        if (idx >= n) break;
        int ry = y + r * rowh;
        if (idx == *sel) gfx_fill(s, x + 1, ry, w - 2, rowh, UI_COL_SEL);
        gfx_str_clip(s, x + 4, ry + 2, items[idx],
                     idx == *sel ? UI_COL_TEXT : UI_COL_MUTED, x + w - 2);
    }
    return changed;
}
