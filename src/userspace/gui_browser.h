/*
 * gui_browser.h -- a small directory-navigation model shared by the Files
 * client, the Editor client's open/save dialog, and the server's `fstest`.
 *
 * It keeps its OWN cwd string and re-establishes it via sys_chdir before every
 * read, so two browsers (e.g. Files + a save dialog) can be live at once
 * without fighting over the process-wide cwd.  All filesystem access is via the
 * existing chdir/readdir/getcwd syscalls -- no new ABI.
 */
#ifndef GUI_BROWSER_H
#define GUI_BROWSER_H

#include "gui_ui.h"     /* ui_ctx + gfx_surface, for the shared file dialog */
#include "makx.h"       /* mx_conn, for the windowed dialog (br_dialog_window) */

#define BR_MAX  256
#define BR_NAMW 64
typedef struct {
    char          cwd[256];
    char          name[BR_MAX][BR_NAMW];   /* display names; dirs end with '/' */
    unsigned char type[BR_MAX];
    const char   *ptr[BR_MAX];             /* listbox item pointers            */
    int           n, sel, scroll, loaded;
    int           view;                     /* 0 = list, 1 = icon grid           */
    char          pathedit[256];            /* editable path box (file dialog)   */
} browser;

void br_load(browser *b);                  /* (re)read b->cwd into the model    */
int  br_sel_isdir(browser *b);
void br_up(browser *b);                    /* navigate to parent + reload       */
void br_enter_sel(browser *b);             /* descend into the selected dir     */
void br_sel_path(browser *b, char *out, int max);   /* abs path of selection    */
void br_join(browser *b, const char *leaf, char *out, int max); /* cwd/leaf      */
int  br_goto(browser *b, const char *path, char *out, int max); /* type-a-path: 0 dir (navigated), 1 file (out set) */

/* path helpers (used by the open/save dialog) */
void path_dir(const char *path, char *out, int max);   /* dir part (def "/")    */
void path_base(const char *path, char *out, int max);  /* file part             */

/* Reusable open/save file dialog -- a shared component any windowed app can
 * drop into a surface rect (editor, a future save-page action in a browser,
 * etc.).  Draws the toolbar (Up / Open|Save / Cancel), an optional Name field
 * (save mode), and the directory listbox, and handles navigation + selection.
 * Needs a gui_ui ui_ctx (the caller's per-frame input snapshot).
 *
 *   mode      1 = open, 2 = save-as
 *   savename  caller-owned save-as filename field (used only in save mode)
 *   out       receives the chosen absolute path on accept
 * Returns: 0 still open, 1 accepted (out is set), 2 cancelled.
 */
int br_dialog(browser *b, ui_ctx *u, gfx_surface *s,
              int x, int y, int w, int h, int mode,
              char *savename, int savecap, char *out, int outcap);

/* Run the open/save dialog as its own centred window (makx multi-window): opens
 * a transient dialog window off `parent`, drives br_dialog in its own event
 * loop until the user accepts or cancels, then closes the window (the parent app
 * keeps running).  `mode` 1=open / 2=save.  Returns 1 (accepted, out set),
 * 2 (cancelled / window closed), or -1 if the window couldn't be opened. */
int br_dialog_window(const mx_conn *parent, browser *b, int mode,
                     char *savename, int savecap, char *out, int outcap);

#endif /* GUI_BROWSER_H */
