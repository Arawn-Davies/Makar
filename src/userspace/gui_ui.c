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

int ui_btn_w(const char *label)
{
    int w = gfx_text_w(label) + 2 * UI_BTN_PADX;
    return w < 36 ? 36 : w;
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

    if (hot && c->mpressed) { c->focus = id; c->got_input = 1; c->tb_selall = 0; }

    if (c->focus == id && c->key >= 0) {
        int k = c->key, len = ui_strlen(buf);
        /* Clipboard shortcuts on every textbox: Ctrl-A/C/X/V.  Selection is
         * all-or-nothing (Ctrl-A), which covers the common cases -- select-all
         * then copy/replace/clear -- without a full caret+range model. */
        if (k == 1) {                             /* Ctrl-A: select all */
            c->tb_selall = (len > 0);
        } else if (k == 3) {                      /* Ctrl-C: copy selection */
            if (!masked && c->tb_selall && len > 0) sys_clip_set(buf, (unsigned)len);
        } else if (k == 24) {                     /* Ctrl-X: cut selection */
            if (!masked && c->tb_selall && len > 0) { sys_clip_set(buf, (unsigned)len); buf[0] = 0; changed = 1; }
            c->tb_selall = 0;
        } else if (k == 22) {                     /* Ctrl-V: paste at end / over selection */
            char cb[256];
            int n = sys_clip_get(cb, sizeof cb);
            if (n > (int)sizeof cb) n = (int)sizeof cb;
            int wpos = c->tb_selall ? 0 : len;
            for (int i = 0; i < n && wpos < cap - 1; i++) {
                char ch = cb[i];
                if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';  /* single-line */
                buf[wpos++] = ch;
            }
            buf[wpos] = 0; changed = 1; c->tb_selall = 0;
        } else if (k == 8 || k == 127) {          /* backspace */
            if (c->tb_selall) { buf[0] = 0; c->tb_selall = 0; changed = 1; }
            else if (len > 0) { buf[len - 1] = 0; changed = 1; }
        } else if (k >= 32 && k < 127) {          /* printable (replaces a selection) */
            if (c->tb_selall) { buf[0] = 0; len = 0; c->tb_selall = 0; }
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
    if (focused && c->tb_selall && shown > 0)      /* selection highlight */
        gfx_fill(s, tx, ty - 1, shown * 8, 10, UI_COL_SEL);
    if (masked) {
        /* One round dot per character (the bitmap font has no bullet glyph, so
         * draw a small filled rounded square that reads as a dot). */
        gfx_u32 dot_bg = focused ? UI_COL_FIELD_FC : UI_COL_FIELD;
        for (int i = 0; i < shown; i++)
            gfx_round(s, tx + i * 8 + 2, ty + 2, 5, 5, UI_COL_TEXT, dot_bg);
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

int ui_vscroll(ui_ctx *c, gfx_surface *s, int x, int y, int w, int h,
               int total, int vis, int *top)
{
    int id = ++c->cur_id;
    int changed = 0;
    if (total < 1) total = 1;
    if (vis   < 1) vis   = 1;
    int maxtop = total - vis; if (maxtop < 0) maxtop = 0;
    if (*top < 0)      *top = 0;
    if (*top > maxtop) *top = maxtop;

    gfx_fill(s, x, y, w, h, UI_COL_TRACK);
    gfx_outline(s, x, y, w, h, UI_COL_BORDER);

    /* thumb height tracks the visible fraction (min 16px so it stays grabbable) */
    int th = (vis >= total) ? h : (h * vis) / total;
    if (th < 16) th = 16;
    if (th > h)  th = h;
    int span = h - th;
    int ty = (maxtop > 0) ? y + (span * (*top)) / maxtop : y;

    /* press anywhere in the track grabs; drag while held repositions the thumb */
    if (maxtop > 0 && c->mpressed && pt_in(c, x, y, w, h)) { c->active = id; c->got_input = 1; }
    if (c->active == id) {
        if (c->mdown && span > 0) {
            int ny = c->my - th / 2 - y;
            if (ny < 0)    ny = 0;
            if (ny > span) ny = span;
            int nt = (ny * maxtop) / span;
            if (nt != *top) { *top = nt; changed = 1; }
            ty = y + (span * (*top)) / maxtop;
        }
        if (c->mreleased) c->active = 0;
    }

    gfx_round(s, x + 1, ty + 1, w - 2, th - 2,
              (maxtop > 0) ? UI_COL_BTN_ACT : UI_COL_BTN, UI_COL_BORDER);
    return changed;
}

/* ---- menus -------------------------------------------------------------- */

/* Shared dropdown/popup body: draw `items` as a list anchored at (x,y), width
 * auto-fitting the longest label (+ an accel column).  Returns a clicked item's
 * action (> 0), else 0; sets *outside when the click landed off the popup. */
static int ui_popup_list(ui_ctx *c, gfx_surface *s, int x, int y,
                         const ui_menu_item *items, int n, int *outside)
{
    const int rowh = 16, padx = 12, gap = 18;
    int hasacc = 0, maxlw = 0, maxaw = 0;
    for (int i = 0; i < n; i++) {
        if (!items[i].label) continue;
        int lw = gfx_text_w(items[i].label); if (lw > maxlw) maxlw = lw;
        if (items[i].accel) { hasacc = 1; int aw = gfx_text_w(items[i].accel); if (aw > maxaw) maxaw = aw; }
    }
    int w = padx * 2 + maxlw + (hasacc ? gap + maxaw : 0);
    if (w < 100) w = 100;
    int h = 4;
    for (int i = 0; i < n; i++) h += items[i].label ? rowh : 6;
    if (x + w > s->w) x = s->w - w;
    if (x < 0) x = 0;
    if (y + h > s->h) y = s->h - h;
    if (y < 0) y = 0;

    gfx_fill(s, x, y, w, h, UI_COL_FIELD_FC);
    gfx_outline(s, x, y, w, h, UI_COL_BORDER);

    int action = 0, cy = y + 2;
    for (int i = 0; i < n; i++) {
        if (!items[i].label) { gfx_fill(s, x + 5, cy + 2, w - 10, 1, UI_COL_BORDER); cy += 6; continue; }
        int hot = pt_in(c, x, cy, w, rowh) && items[i].enabled;
        if (hot) gfx_fill(s, x + 1, cy, w - 2, rowh, UI_COL_SEL);
        gfx_u32 fg = items[i].enabled ? UI_COL_TEXT : UI_COL_MUTED;
        gfx_str_clip(s, x + padx, cy + 4, items[i].label, fg, x + w - 2);
        if (items[i].accel)
            gfx_str_clip(s, x + w - padx - gfx_text_w(items[i].accel), cy + 4,
                         items[i].accel, UI_COL_MUTED, x + w - 2);
        if (hot && c->mpressed) { action = items[i].action; c->got_input = 1; }
        cy += rowh;
    }
    if (c->mpressed && !pt_in(c, x, y, w, h)) *outside = 1;
    return action;
}

int ui_menubar(ui_ctx *c, gfx_surface *s, const ui_menu *menus, int n, int *open)
{
    if (n > 16) n = 16;
    int prev_open = *open;

    gfx_fill(s, 0, 0, s->w, UI_MENUBAR_H, UI_COL_BTN);
    gfx_fill(s, 0, UI_MENUBAR_H - 1, s->w, 1, UI_COL_BORDER);

    int title_x[16];
    int clicked = -1, tx = 4;
    for (int i = 0; i < n; i++) {
        int tw  = gfx_text_w(menus[i].title) + 16;
        int hot = pt_in(c, tx, 0, tw, UI_MENUBAR_H);
        int isopen = (prev_open == i);
        if (isopen || hot)
            gfx_fill(s, tx, 0, tw, UI_MENUBAR_H, isopen ? UI_COL_BTN_ACT : UI_COL_BTN_HOT);
        gfx_str(s, tx + 8, (UI_MENUBAR_H - 8) / 2, menus[i].title, UI_COL_TEXT);
        if (hot && c->mpressed) { clicked = i; c->got_input = 1; }
        title_x[i] = tx;
        tx += tw;
    }

    int action = 0, outside = 0;
    if (prev_open >= 0 && prev_open < n)
        action = ui_popup_list(c, s, title_x[prev_open], UI_MENUBAR_H,
                               menus[prev_open].items, menus[prev_open].n, &outside);

    if (clicked >= 0)              *open = (clicked == prev_open) ? -1 : clicked; /* toggle/switch */
    else if (action || outside)    *open = -1;                                   /* pick / click-away */
    return action;
}

int ui_context_menu(ui_ctx *c, gfx_surface *s, int x, int y,
                    const ui_menu_item *items, int n, int *open)
{
    if (!*open) return 0;
    int outside = 0;
    int action = ui_popup_list(c, s, x, y, items, n, &outside);
    if (action || outside) *open = 0;
    return action;
}

int ui_about(ui_ctx *c, gfx_surface *s, const char *title,
             const char *const *lines, int nlines, int *open)
{
    if (!*open) return 0;
    const int lh = 12, w = 380;
    int h = 38 + nlines * lh + 40;
    int x = (s->w - w) / 2, y = (s->h - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    gfx_fill(s, x, y, w, h, UI_COL_FIELD_FC);
    gfx_outline(s, x, y, w, h, UI_COL_BTN_ACT);
    gfx_str(s, x + 14, y + 12, title, UI_COL_TEXT);
    gfx_fill(s, x + 14, y + 26, w - 28, 1, UI_COL_BORDER);

    int ly = y + 34;
    for (int i = 0; i < nlines; i++) {
        gfx_str_clip(s, x + 14, ly, lines[i], UI_COL_MUTED, x + w - 14);
        ly += lh;
    }

    int bw = 70, bh = 22, bx = x + (w - bw) / 2, by = y + h - bh - 9;
    int ok = ui_button(c, s, bx, by, bw, bh, "OK");
    /* Clicking anywhere outside the card also dismisses (Windows-ish). */
    if (ok || (c->mpressed && !pt_in(c, x, y, w, h))) { *open = 0; return 1; }
    return 0;
}

void ui_gate_begin(ui_ctx *c, ui_gate *g, int busy)
{
    g->key = c->key; g->mp = c->mpressed; g->md = c->mdown; g->mr = c->mreleased;
    if (busy) { c->key = -1; c->mpressed = c->mdown = c->mreleased = 0; }
}
void ui_gate_end(ui_ctx *c, const ui_gate *g)
{
    c->key = g->key; c->mpressed = g->mp; c->mdown = g->md; c->mreleased = g->mr;
}

int ui_appbar(ui_ctx *c, gfx_surface *s, const char *app,
              const char *const *about, int nabout,
              const ui_menu *extra, int nextra,
              int *open, int *about_open, int *quit)
{
    enum { AB_EXIT = 1000000, AB_ABOUT = 1000001 };  /* private; won't clash with app enums */
    static const ui_menu_item fitems[] = { { "Exit", AB_EXIT, 1, 0 } };

    char hl[64];                                       /* "About <app>" (Help item + title) */
    { int n = 0; const char *p = "About ";
      while (*p && n < 62) hl[n++] = *p++;
      for (const char *q = app; *q && n < 63; q++) hl[n++] = *q;
      hl[n] = 0; }
    ui_menu_item hitems[1];
    hitems[0].label = hl; hitems[0].action = AB_ABOUT; hitems[0].enabled = 1; hitems[0].accel = 0;

    ui_menu menus[18];
    int n = 0;
    menus[n].title = "File"; menus[n].items = fitems; menus[n].n = 1; n++;
    for (int i = 0; i < nextra && n < 16; i++) menus[n++] = extra[i];
    menus[n].title = "Help"; menus[n].items = hitems; menus[n].n = 1; n++;

    int act = ui_menubar(c, s, menus, n, open);
    if      (act == AB_EXIT)  { if (quit) *quit = 1; act = 0; }
    else if (act == AB_ABOUT) { if (about_open) *about_open = 1; act = 0; }

    if (about_open) ui_about(c, s, hl, about, nabout, about_open);
    return act;
}
