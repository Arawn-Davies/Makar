/*
 * gui_browser.c -- directory-navigation model (see gui_browser.h).  Lifted
 * verbatim from the old monolithic wm.c so the Files/Editor clients and the
 * server's fstest all share one implementation.
 */
#include "syscall.h"
#include "gui_browser.h"

static int   slen(const char *s){ int n=0; while(s[n]) n++; return n; }
static void  scpy(char *d, const char *s, int max){ int i=0; while(s[i]&&i<max-1){d[i]=s[i];i++;} d[i]=0; }
static char *scat(char *d, const char *s){ while(*d)d++; while(*s)*d++=*s++; *d=0; return d; }

void br_load(browser *b)
{
    if (!b->cwd[0]) scpy(b->cwd, "/", sizeof b->cwd);
    sys_chdir(b->cwd);                      /* make this browser's cwd current  */
    b->n = 0;
    struct dirent de;
    for (unsigned i=0; b->n<BR_MAX; i++){
        if (sys_readdir(".", i, &de)!=1) break;
        /* hide "." and ".." -- the Up button handles parent navigation */
        if (de.d_name[0]=='.' && (de.d_name[1]==0 || (de.d_name[1]=='.'&&de.d_name[2]==0))) continue;
        scpy(b->name[b->n], de.d_name, BR_NAMW);
        if (de.d_type==DT_DIR){ int l=slen(b->name[b->n]); if(l<BR_NAMW-2){b->name[b->n][l]='/';b->name[b->n][l+1]=0;} }
        b->type[b->n]=de.d_type;
        b->n++;
    }
    sys_getcwd(b->cwd, sizeof b->cwd);       /* normalised absolute cwd          */
    for (int i=0;i<b->n;i++) b->ptr[i]=b->name[i];
    if (b->sel>=b->n) b->sel = b->n?b->n-1:0;
    scpy(b->pathedit, b->cwd, sizeof b->pathedit);   /* sync the editable path box */
    b->loaded=1;
}

/* Navigate the file dialog to a typed path.  If it's a directory, descend into
 * it (stay in the dialog) and return 0; otherwise hand it back as the chosen
 * file path (return 1) for the caller to open. */
int br_goto(browser *b, const char *path, char *out, int outcap)
{
    sys_chdir(b->cwd);
    if (path[0] && sys_chdir(path)==0){
        sys_getcwd(b->cwd, sizeof b->cwd);
        b->sel=b->scroll=0; b->loaded=0; br_load(b);
        return 0;
    }
    scpy(out, path, outcap);                  /* not a dir -> treat as a file */
    return 1;
}

int br_sel_isdir(browser *b){ return b->sel>=0 && b->sel<b->n && b->type[b->sel]==DT_DIR; }

/* Navigate then capture the resulting absolute cwd back into b->cwd *before*
 * reloading -- otherwise br_load's own sys_chdir(b->cwd) would snap us back to
 * the pre-navigation directory (this was the "Files does nothing" bug). */
void br_up(browser *b)
{
    sys_chdir(b->cwd); sys_chdir("..");
    sys_getcwd(b->cwd, sizeof b->cwd);
    b->sel=b->scroll=0; br_load(b);
}
void br_enter_sel(browser *b)
{
    if (!br_sel_isdir(b)) return;
    char nm[BR_NAMW]; scpy(nm, b->name[b->sel], BR_NAMW);
    int l=slen(nm); if(l>0 && nm[l-1]=='/') nm[l-1]=0;
    sys_chdir(b->cwd); sys_chdir(nm);
    sys_getcwd(b->cwd, sizeof b->cwd);
    b->sel=b->scroll=0; br_load(b);
}
void br_sel_path(browser *b, char *out, int max)
{
    scpy(out, b->cwd, max);
    int l=slen(out); if(l>0 && out[l-1]!='/' && l<max-2){ out[l]='/'; out[l+1]=0; }
    char nm[BR_NAMW]; scpy(nm, (b->sel>=0&&b->sel<b->n)?b->name[b->sel]:"", BR_NAMW);
    int nl=slen(nm); if(nl>0 && nm[nl-1]=='/') nm[nl-1]=0;
    scat(out, nm);
}
void br_join(browser *b, const char *leaf, char *out, int max)
{
    scpy(out, b->cwd, max);
    int l=slen(out); if(l>0 && out[l-1]!='/' && l<max-2){ out[l]='/'; out[l+1]=0; }
    scat(out, leaf);
}

void path_dir(const char *path, char *out, int max)
{
    int last=-1; for(int i=0; path[i]; i++) if(path[i]=='/') last=i;
    if (last<=0){ scpy(out,"/",max); return; }
    int n = last<max-1?last:max-1; for(int i=0;i<n;i++) out[i]=path[i]; out[n]=0;
}
void path_base(const char *path, char *out, int max)
{
    int last=-1; for(int i=0; path[i]; i++) if(path[i]=='/') last=i;
    scpy(out, path+last+1, max);
}

/* Icon-grid view of the directory (the alternative to the listbox).  Draws a
 * folder/file glyph + name per item with a scrollbar; sets b->sel on click and
 * returns 1 when an already-selected item is clicked (the "activate" gesture,
 * mirroring the listbox's click-on-selected). */
static int br_icons(browser *b, ui_ctx *u, gfx_surface *s, int x, int y, int w, int h, int prev)
{
    gfx_fill(s, x, y, w, h, UI_COL_FIELD);
    int cellw=90, cellh=60;
    int cols=(w-12)/cellw; if (cols<1) cols=1;
    int rows=(b->n+cols-1)/cols; if (rows<1) rows=1;
    int visrows=h/cellh; if (visrows<1) visrows=1;
    if (b->scroll>rows-visrows) b->scroll = rows-visrows>0 ? rows-visrows : 0;
    if (b->scroll<0) b->scroll=0;

    int activate=0;
    for (int idx=0; idx<b->n; idx++){
        int r=idx/cols, c=idx%cols;
        int cy=y + (r-b->scroll)*cellh;
        if (cy+cellh<=y || cy>=y+h) continue;
        int cxp=x + c*cellw;
        int isdir=(b->type[idx]==DT_DIR);
        if (idx==b->sel) gfx_round(s, cxp+2, cy+2, cellw-6, cellh-6, UI_COL_SEL, UI_COL_BTN_ACT);
        int ix=cxp+(cellw-32)/2, iy=cy+6;
        if (isdir){
            gfx_fill(s, ix+2, iy, 12, 4, GFX_RGB(0xE0,0xB0,0x40));
            gfx_round(s, ix, iy+3, 30, 20, GFX_RGB(0xF0,0xC8,0x50), UI_COL_BORDER);
        } else {
            gfx_round(s, ix+5, iy, 20, 26, GFX_RGB(0xCE,0xD6,0xE0), UI_COL_BORDER);
            gfx_fill(s, ix+9, iy+6, 12, 2, UI_COL_MUTED);
            gfx_fill(s, ix+9, iy+11,12, 2, UI_COL_MUTED);
            gfx_fill(s, ix+9, iy+16, 9, 2, UI_COL_MUTED);
        }
        gfx_str_clip(s, cxp+4, cy+34, b->name[idx], UI_COL_TEXT, cxp+cellw-4);
        if (u->mpressed && u->mx>=cxp && u->mx<cxp+cellw && u->my>=cy && u->my<cy+cellh
            && u->my>=y && u->my<y+h){
            u->got_input=1;
            if (prev==idx) activate=1;     /* click an already-selected item = open */
            b->sel=idx;
        }
    }
    ui_vscroll(u, s, x+w-10, y, 10, h, rows, visrows, &b->scroll);
    return activate;
}

/* Shared open/save file dialog (see gui_browser.h).  Pure function of the
 * browser model + a per-frame ui_ctx -- any windowed client can drop it into a
 * rect of its surface.  Lifted out of the editor so it's reusable. */
int br_dialog(browser *b, ui_ctx *u, gfx_surface *s,
              int x, int y, int w, int h, int mode,
              char *savename, int savecap, char *out, int outcap)
{
    gfx_fill(s, x, y, w, h, UI_COL_FIELD);
    gfx_outline(s, x, y, w, h, UI_COL_BTN_ACT);
    int bx=x+6, by=y+6;
    int up_c   = ui_button(u, s, bx,     by, 52, 20, "Up");
    int act_c  = ui_button(u, s, bx+60,  by, 70, 20, mode==1?"Open":"Save");
    int can_c  = ui_button(u, s, bx+138, by, 64, 20, "Cancel");
    int view_c = ui_button(u, s, bx+210, by, 56, 20, b->view?"List":"Icons");
    /* editable path box: type a directory to jump to it, or a file to open it
     * (Enter while it's focused), instead of only clicking through the list. */
    if (!b->pathedit[0]) scpy(b->pathedit, b->cwd, sizeof b->pathedit);
    int _pbx = bx+274, _pbw = (x+w-6) - _pbx;
    int _pb_id = u->cur_id + 1;
    ui_textbox(u, s, _pbx, by, _pbw<60?60:_pbw, 20, b->pathedit, sizeof b->pathedit);
    int _pb_focused = (u->focus == _pb_id);
    if (up_c)   { br_up(b); }
    if (view_c) { b->view=!b->view; b->scroll=0; }
    if (can_c)  return 2;
    if (_pb_focused && u->key=='\n'){
        if (br_goto(b, b->pathedit, out, outcap)==1) return 1;   /* a file -> open */
        return 0;                                                /* a dir -> navigated */
    }

    int rowy=by+26, listy=rowy;
    if (mode==2){
        ui_label(u, s, bx, rowy+4, "Name:", UI_COL_MUTED);
        ui_textbox(u, s, bx+48, rowy, w-60-90, 20, savename, savecap);
        listy = rowy+26;
    }
    int lx=x+6, ly=listy, lw=w-12, lh=(y+h)-listy-6, prev=b->sel;
    int activate;
    if (b->view==0){
        ui_listbox(u, s, lx, ly, lw, lh, b->ptr, b->n, &b->sel, &b->scroll);
        activate = act_c || (u->key=='\n')
            || (u->mpressed && prev==b->sel && u->mx>=lx && u->mx<lx+lw && u->my>=ly && u->my<ly+lh);
    } else {
        activate = br_icons(b, u, s, lx, ly, lw, lh, prev) || act_c || (u->key=='\n');
    }

    if (mode==1){
        if (activate){
            if (br_sel_isdir(b)) br_enter_sel(b);
            else if (b->n>0){ br_sel_path(b, out, outcap); return 1; }
        }
    } else {
        if (br_sel_isdir(b) && activate && act_c==0) br_enter_sel(b);
        else if (!br_sel_isdir(b) && (u->mpressed && prev==b->sel)) path_base(b->name[b->sel], savename, savecap);
        if (act_c && savename[0]){ br_join(b, savename, out, outcap); return 1; }
    }
    return 0;
}

/* Run br_dialog as its own centred makx window (see gui_browser.h). */
int br_dialog_window(const mx_conn *parent, browser *b, int mode,
                     char *savename, int savecap, char *out, int outcap)
{
    mx_conn dlg;
    int dw=560, dh=400;
    if (mx_open_window(&dlg, parent, dw, dh, 0) != 0) return -1;
    if (!b->loaded) br_load(b);

    ui_ctx u;
    int result=0;
    while (!dlg.closed){
        mx_pump(&dlg);
        ui_begin(&u, dlg.mx, dlg.my, dlg.mdown, dlg.mpressed, dlg.mreleased, mx_key(&dlg));
        gfx_fill(&dlg.surf, 0, 0, dlg.surf.w, dlg.surf.h, UI_COL_BTN);
        int r = br_dialog(b, &u, &dlg.surf, 0, 0, dlg.surf.w, dlg.surf.h,
                          mode, savename, savecap, out, outcap);
        mx_present(&dlg);
        if (r==1){ result=1; break; }
        if (r==2){ result=2; break; }
        sys_yield();
    }
    if (result==0) result=2;          /* window closed via its X = cancel */
    mx_close(&dlg);                    /* per-window MX_BYE -> server frees the dialog */
    return result;
}
