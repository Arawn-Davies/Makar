/*
 * gui_ui.h -- immediate-mode widget toolkit for the Makar GUI.
 *
 * Classic IMGUI model: there is no retained widget tree.  Each frame the caller
 * snapshots input into a ui_ctx (ui_begin) and then calls ui_button / ui_slider
 * / ui_textbox / ui_listbox in a fixed order; widgets both draw themselves into
 * a target gfx_surface and report interaction.  Widget identity is the call
 * order within the frame, so keep the call sequence stable.  All widget state
 * the caller cares about (button labels, slider/textbox/list values) is
 * caller-owned; the toolkit only tracks transient interaction (which widget is
 * pressed / has keyboard focus).
 *
 * Coordinates are in the target surface's pixel space.  For a window, the
 * caller passes a sub-surface (or offsets) so widgets sit inside the client
 * area; the window manager hit-tests windows first and only feeds the focused
 * window's widgets a "live" ui_ctx (others get a ctx with no mouse buttons /
 * key so they draw but don't react).
 */
#ifndef GUI_UI_H
#define GUI_UI_H

#include "gui_gfx.h"

typedef struct {
    int mx, my;         /* mouse position (target-surface pixels)            */
    int mdown;          /* left button currently held                        */
    int mpressed;       /* left button went down this frame (edge)           */
    int mreleased;      /* left button went up this frame (edge)             */
    int key;            /* one key this frame (ASCII or KEY_* sentinel), -1   */

    /* internal, reset/managed by the toolkit */
    int cur_id;         /* auto-increment widget id within the frame          */
    int active;         /* id of the widget being pressed/dragged (0 = none)  */
    int focus;          /* id of the widget with keyboard focus (0 = none)    */
    int got_input;      /* set when a widget consumed the click/key this frame */
} ui_ctx;

/* Theme colours (XRGB8888). */
#define UI_COL_BTN      GFX_RGB(0x2c,0x3a,0x52)
#define UI_COL_BTN_HOT  GFX_RGB(0x3a,0x4e,0x6e)
#define UI_COL_BTN_ACT  GFX_RGB(0x4c,0x8d,0xff)
#define UI_COL_TEXT     GFX_RGB(0xe6,0xea,0xf0)
#define UI_COL_MUTED    GFX_RGB(0x90,0xa0,0xb5)
#define UI_COL_FIELD    GFX_RGB(0x12,0x16,0x1e)
#define UI_COL_FIELD_FC GFX_RGB(0x1b,0x24,0x33)
#define UI_COL_BORDER   GFX_RGB(0x0a,0x0c,0x10)
#define UI_COL_TRACK    GFX_RGB(0x1b,0x22,0x2e)
#define UI_COL_SEL      GFX_RGB(0x35,0x55,0x88)

/* Begin a frame: stash the input snapshot.  Pass key = -1 when no key. */
void ui_begin(ui_ctx *c, int mx, int my, int mdown, int mpressed,
              int mreleased, int key);

/* A clickable button.  Returns 1 on the frame the click completes. */
int  ui_button(ui_ctx *c, gfx_surface *s, int x, int y, int w, int h,
               const char *label);

/* Horizontal slider over [lo,hi]; *val is read and written.  Returns 1 if the
 * value changed this frame. */
int  ui_slider(ui_ctx *c, gfx_surface *s, int x, int y, int w, int h,
               int *val, int lo, int hi);

/* Single-line text box editing buf[cap].  Click to focus; typing edits the
 * focused box.  Returns 1 if the contents changed this frame. */
int  ui_textbox(ui_ctx *c, gfx_surface *s, int x, int y, int w, int h,
                char *buf, int cap);

/* Like ui_textbox but renders each character as '*' (password entry). */
int  ui_password(ui_ctx *c, gfx_surface *s, int x, int y, int w, int h,
                 char *buf, int cap);

/* Static text. */
void ui_label(ui_ctx *c, gfx_surface *s, int x, int y, const char *str,
              gfx_u32 fg);

/* Scrollable single-select list.  *sel is the selected row (read/written),
 * *scroll is the top row (caller-owned, may be 0).  Click selects a row;
 * when focused, Up/Down move the selection.  Returns 1 if *sel changed. */
int  ui_listbox(ui_ctx *c, gfx_surface *s, int x, int y, int w, int h,
                const char *const *items, int n, int *sel, int *scroll);

#endif /* GUI_UI_H */
