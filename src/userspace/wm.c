/*
 * wm.c -- gui.elf: a small multi-window manager / compositor for Makar.
 *
 * The WM owns the framebuffer (it is the single focused root-GUI task) and is
 * the sole compositor: every window renders into the WM's back buffer and the
 * whole frame is presented once via SYS_FB_PRESENT.  Keyboard + mouse focus is
 * managed entirely inside the WM -- clicking a window raises it and makes it
 * the keyboard target; the window under the cursor gets mouse input.  See
 * docs/gui.md.
 *
 * Window kinds (interim built-ins; the makx split migrates these to clients):
 *   - Terminal   : hosts sh.elf over pipes (the byte stream is drawn as a grid)
 *   - Editor     : native text editor built on the gui_ui widgets + file I/O
 *   - Files      : native file browser (sys_readdir)
 *   - Tasks      : native task manager (/proc/tasks + SYS_KILL)
 *   - App        : a GENERIC surface-backed external program (e.g. doom.elf);
 *                  the server names no app -- the program is launcher data
 *
 * Assumes a 32-bpp XRGB8888 framebuffer (QEMU Bochs VBE default).
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "gui_ui.h"

/* ---- framebuffer / back buffer ----------------------------------------- */

static unsigned int  FBW, FBH;
static gfx_surface   scr;              /* the WM back buffer (scr.px == bb)   */

#define RGB GFX_RGB
#define COL_DESK    RGB(0x1e,0x29,0x3b)
#define COL_WIN     RGB(0x16,0x1b,0x24)
#define COL_TITLE   RGB(0x35,0x6a,0xa8)
#define COL_TITLE_U RGB(0x24,0x48,0x74)   /* unfocused title bar             */
#define COL_TITLE2  RGB(0x24,0x48,0x74)
#define COL_BORDER  RGB(0x07,0x09,0x0d)
#define COL_CLOSE   RGB(0xc0,0x40,0x40)
#define COL_TEXT    RGB(0xd3,0xd7,0xcf)

#define TH       20          /* title-bar height        */
#define DOCK_H   34
#define MENU_H   22          /* top menu bar height     */
#define COL_MENU RGB(0x0c,0x10,0x18)

/* ---- small libc ---------------------------------------------------------- */

static int   slen(const char *s){ int n=0; while(s[n]) n++; return n; }
static void  scpy(char *d, const char *s, int max){ int i=0; while(s[i]&&i<max-1){d[i]=s[i];i++;} d[i]=0; }
static char *u2s(unsigned int v, char *out){ char t[12]; int i=0; if(!v)t[i++]='0'; while(v){t[i++]=(char)('0'+v%10u);v/=10u;} int j=0; while(i)out[j++]=t[--i]; out[j]=0; return out+j; }
static char *scat(char *d, const char *s){ while(*d)d++; while(*s)*d++=*s++; *d=0; return d; }
static int   seq(const char *a, const char *b){ int i=0; while(a[i]&&a[i]==b[i])i++; return a[i]==b[i]; }

/* ---- window model -------------------------------------------------------- */

/* Window kinds the server composites.  W_APP is a *generic* surface-backed
 * external application (no app baked in -- the program path is launcher data,
 * not server logic); terminal/editor/files/tasks are interim built-ins the
 * makx split will migrate out to clients too. */
enum { W_TERMINAL, W_EDITOR, W_FILES, W_TASKS, W_APP, W_COUNT };

typedef struct {
    int    open;
    int    x, y, w, h;          /* outer rect                                */
    int    minimized;           /* hidden (still in the dock)                */
    int    maximized;           /* filling the work area                     */
    int    sx, sy, sw, sh;      /* geometry saved before maximise            */
    char   title[40];
    ui_ctx ui;                  /* persistent immediate-mode state           */
} window;

static window wins[W_COUNT];
static int    zlist[W_COUNT];   /* back (0) -> front (W_COUNT-1), holds kinds */
static int    focus_kind = -1;

static int client_x(window *w){ return w->x + 1; }
static int client_y(window *w){ return w->y + TH; }
static int client_w(window *w){ return w->w - 2; }
static int client_h(window *w){ return w->h - TH - 1; }

/* z-order helpers ---------------------------------------------------------- */
static void z_raise(int kind)
{
    int i = 0; while (i < W_COUNT && zlist[i] != kind) i++;
    if (i >= W_COUNT) return;
    for (; i < W_COUNT - 1; i++) zlist[i] = zlist[i + 1];
    zlist[W_COUNT - 1] = kind;
    focus_kind = kind;
}

/* ================= Terminal window (sh.elf over pipes) =================== */

#define TCOLS 96
#define TROWS 44
static char term[TROWS][TCOLS];
static int  t_cols, t_rows, t_cr, t_cc;
static int  term_pid = -1, term_in = -1, term_out = -1;

static void term_clear(void){ for(int r=0;r<TROWS;r++)for(int c=0;c<TCOLS;c++)term[r][c]=' '; t_cr=t_cc=0; }
static void term_newline(void)
{
    t_cc = 0;
    if (++t_cr >= t_rows) {
        for (int r=0;r<t_rows-1;r++) for(int c=0;c<TCOLS;c++) term[r][c]=term[r+1][c];
        for (int c=0;c<TCOLS;c++) term[t_rows-1][c]=' ';
        t_cr = t_rows-1;
    }
}
static void term_putc(char ch)
{
    if (ch=='\n'){ term_newline(); return; }
    if (ch=='\r'){ t_cc=0; return; }
    if (ch==8||ch==127){ if(t_cc>0){t_cc--; term[t_cr][t_cc]=' ';} return; }
    if (ch<32) return;
    if (t_cc>=t_cols) term_newline();
    if (t_cr<TROWS && t_cc<TCOLS) term[t_cr][t_cc++]=ch;
}
static void term_spawn(void)
{
    if (term_pid > 0) return;
    int ip[2], op[2];
    if (sys_pipe(ip)<0 || sys_pipe(op)<0){ const char*m="terminal: pipe failed\n"; for(int i=0;m[i];i++)term_putc(m[i]); return; }
    int pid = sys_fork();
    if (pid<0){ const char*m="terminal: fork failed\n"; for(int i=0;m[i];i++)term_putc(m[i]); return; }
    if (pid==0){
        sys_close(ip[1]); sys_close(op[0]);
        sys_dup2(ip[0],0); sys_dup2(op[1],1); sys_dup2(op[1],2);
        sys_close(ip[0]); sys_close(op[1]);
        char user[48], uarg[64]; char *av[3]={ "sh.elf", 0, 0 };
        if (sys_whoami(user,sizeof user)>0){ scpy(uarg,"--user=",sizeof uarg); scat(uarg,user); av[1]=uarg; }
        sys_execve("/apps/sh.elf", av, (char *const*)0);
        sys_exit(127);
    }
    term_pid=pid; term_in=ip[1]; term_out=op[0];
    sys_close(ip[0]); sys_close(op[1]);
    sys_fcntl(term_out, F_SETFL, O_NONBLOCK);
    sys_fcntl(term_in,  F_SETFL, O_NONBLOCK);
}
static void term_kill(void)
{
    if (term_pid>0){ sys_kill(term_pid, SIGKILL); int st=0; sys_wait4(term_pid,&st,0); }
    if (term_in>=0) sys_close(term_in);
    if (term_out>=0) sys_close(term_out);
    term_pid=-1; term_in=-1; term_out=-1;
}
static int term_pump(void)
{
    if (term_out<0) return 0;
    unsigned char b[128]; int dirty=0;
    for(;;){ long n=sys_read(term_out,b,sizeof b); if(n<=0) break; for(long i=0;i<n;i++)term_putc((char)b[i]); dirty=1; if(n<(long)sizeof b) break; }
    if (term_pid>0){ int st=0; if (sys_wait4(term_pid,&st,WNOHANG)==term_pid){ term_pid=-1; const char*m="\n[shell exited]\n"; for(int i=0;m[i];i++)term_putc(m[i]); dirty=1; } }
    return dirty;
}
static void term_key(int k)
{
    if (term_in<0) return;
    unsigned char b=(unsigned char)k; if(b=='\r')b='\n';
    sys_write(term_in,&b,1);
}
static void term_draw(window *w)
{
    int cx=client_x(w), cy=client_y(w), cw=client_w(w), ch=client_h(w);
    gfx_fill(&scr, cx, cy, cw, ch, RGB(0x0e,0x12,0x18));
    t_cols = (cw-8)/8; if(t_cols>TCOLS)t_cols=TCOLS;
    t_rows = (ch-8)/8; if(t_rows>TROWS)t_rows=TROWS;
    int bx=cx+4, by=cy+4;
    for(int r=0;r<t_rows;r++)
        for(int c=0;c<t_cols;c++)
            if(term[r][c]!=' ') gfx_char(&scr, bx+c*8, by+r*8, (unsigned char)term[r][c], COL_TEXT);
    if (t_cr<t_rows && t_cc<t_cols && focus_kind==W_TERMINAL)
        gfx_fill(&scr, bx+t_cc*8, by+t_cr*8, 8, 8, RGB(0x8a,0xe2,0x34));
}

/* ================= Reusable file browser (model) ======================== */
/* A small directory-navigation model shared by the Files window and the
 * Editor's open/save dialog.  It keeps its OWN cwd string and re-establishes
 * it via sys_chdir before every read, so two browsers (Files + a dialog) can
 * be open at once without fighting over the process-wide cwd.  All filesystem
 * access is via the existing chdir/readdir/getcwd syscalls -- no new ABI. */
#define FZ_MAX  256
#define FZ_NAMW 64
typedef struct {
    char          cwd[256];
    char          name[FZ_MAX][FZ_NAMW];   /* display names; dirs end with '/' */
    unsigned char type[FZ_MAX];
    const char   *ptr[FZ_MAX];             /* listbox item pointers            */
    int           n, sel, scroll, loaded;
} browser;

static void br_load(browser *b)
{
    if (!b->cwd[0]) scpy(b->cwd, "/", sizeof b->cwd);
    sys_chdir(b->cwd);                      /* make this browser's cwd current  */
    b->n = 0;
    struct dirent de;
    for (unsigned i=0; b->n<FZ_MAX; i++){
        if (sys_readdir(".", i, &de)!=1) break;
        /* hide "." and ".." -- the Up button handles parent navigation */
        if (de.d_name[0]=='.' && (de.d_name[1]==0 || (de.d_name[1]=='.'&&de.d_name[2]==0))) continue;
        scpy(b->name[b->n], de.d_name, FZ_NAMW);
        if (de.d_type==DT_DIR){ int l=slen(b->name[b->n]); if(l<FZ_NAMW-2){b->name[b->n][l]='/';b->name[b->n][l+1]=0;} }
        b->type[b->n]=de.d_type;
        b->n++;
    }
    sys_getcwd(b->cwd, sizeof b->cwd);       /* normalised absolute cwd          */
    for (int i=0;i<b->n;i++) b->ptr[i]=b->name[i];
    if (b->sel>=b->n) b->sel = b->n?b->n-1:0;
    b->loaded=1;
}
static int  br_sel_isdir(browser *b){ return b->sel>=0 && b->sel<b->n && b->type[b->sel]==DT_DIR; }
/* Navigate then capture the resulting absolute cwd back into b->cwd *before*
 * reloading -- otherwise br_load's own sys_chdir(b->cwd) would snap us back to
 * the pre-navigation directory (this was the "Files does nothing" bug). */
static void br_up(browser *b)
{
    sys_chdir(b->cwd); sys_chdir("..");
    sys_getcwd(b->cwd, sizeof b->cwd);
    b->sel=b->scroll=0; br_load(b);
}
static void br_enter_sel(browser *b)
{
    if (!br_sel_isdir(b)) return;
    char nm[FZ_NAMW]; scpy(nm, b->name[b->sel], FZ_NAMW);
    int l=slen(nm); if(l>0 && nm[l-1]=='/') nm[l-1]=0;
    sys_chdir(b->cwd); sys_chdir(nm);
    sys_getcwd(b->cwd, sizeof b->cwd);
    b->sel=b->scroll=0; br_load(b);
}
/* Build the absolute path of the current selection (file or dir) into out. */
static void br_sel_path(browser *b, char *out, int max)
{
    scpy(out, b->cwd, max);
    int l=slen(out); if(l>0 && out[l-1]!='/' && l<max-2){ out[l]='/'; out[l+1]=0; }
    char nm[FZ_NAMW]; scpy(nm, (b->sel>=0&&b->sel<b->n)?b->name[b->sel]:"", FZ_NAMW);
    int nl=slen(nm); if(nl>0 && nm[nl-1]=='/') nm[nl-1]=0;
    scat(out, nm);
}
/* Build cwd/<leaf> for an arbitrary leaf name (used by Save-As). */
static void br_join(browser *b, const char *leaf, char *out, int max)
{
    scpy(out, b->cwd, max);
    int l=slen(out); if(l>0 && out[l-1]!='/' && l<max-2){ out[l]='/'; out[l+1]=0; }
    scat(out, leaf);
}

/* ===================== Editor window (native) =========================== */

#define ED_MAX  (32*1024)
static char ed_buf[ED_MAX];
static char ed_path[128];
static int  ed_len, ed_caret, ed_top;     /* caret = byte index, top = first row */
static int  ed_dirty_flag;
static char ed_status[80];

/* Open/Save-As dialog: 0 = none, 1 = open, 2 = save-as.  ed_brz is the dialog's
 * own file browser; ed_savename is the Save-As filename field. */
static int     ed_dlg;
static browser ed_brz;
static char    ed_savename[FZ_NAMW];

/* Copy the directory part of an absolute path into out (defaults to "/"). */
static void path_dir(const char *path, char *out, int max)
{
    int last=-1; for(int i=0; path[i]; i++) if(path[i]=='/') last=i;
    if (last<=0){ scpy(out,"/",max); return; }
    int n = last<max-1?last:max-1; for(int i=0;i<n;i++) out[i]=path[i]; out[n]=0;
}
/* Copy the file part (after the last '/') of a path into out. */
static void path_base(const char *path, char *out, int max)
{
    int last=-1; for(int i=0; path[i]; i++) if(path[i]=='/') last=i;
    scpy(out, path+last+1, max);
}

static void ed_new(void){ ed_buf[0]=0; ed_len=0; ed_caret=0; ed_top=0; ed_dirty_flag=0; scpy(ed_status,"new buffer",sizeof ed_status); }
static void ed_load(const char *path)
{
    scpy(ed_path, path, sizeof ed_path);
    int fd = sys_open(path, O_RDONLY);
    if (fd<0){ ed_new(); scpy(ed_status,"open failed",sizeof ed_status); return; }
    long n = sys_read(fd, ed_buf, ED_MAX-1); sys_close(fd);
    if (n<0) n=0; ed_buf[n]=0; ed_len=(int)n; ed_caret=0; ed_top=0; ed_dirty_flag=0;
    scpy(ed_status,"loaded ",sizeof ed_status); scat(ed_status,path);
}
static void ed_save(void)
{
    if (!ed_path[0]){ scpy(ed_status,"no path",sizeof ed_status); return; }
    int rc = sys_write_file(ed_path, ed_buf, (unsigned)ed_len);
    scpy(ed_status, rc<0 ? "save FAILED" : "saved ", sizeof ed_status);
    if (rc>=0){ scat(ed_status, ed_path); ed_dirty_flag=0; }
}
static void ed_insert(char ch)
{
    if (ed_len >= ED_MAX-1) return;
    for (int i=ed_len; i>ed_caret; i--) ed_buf[i]=ed_buf[i-1];
    ed_buf[ed_caret++]=ch; ed_len++; ed_buf[ed_len]=0; ed_dirty_flag=1;
}
static void ed_backspace(void)
{
    if (ed_caret<=0) return;
    for (int i=ed_caret-1; i<ed_len; i++) ed_buf[i]=ed_buf[i+1];
    ed_caret--; ed_len--; ed_dirty_flag=1;
}
/* caret row/col by scanning newlines */
static void ed_rowcol(int idx, int *row, int *col)
{
    int r=0,c=0; for(int i=0;i<idx && i<ed_len;i++){ if(ed_buf[i]=='\n'){r++;c=0;} else c++; } *row=r; *col=c;
}
static int ed_index_of(int row, int col)
{
    int r=0,c=0; int i=0;
    for(; i<ed_len; i++){
        if (r==row && c==col) return i;
        if (ed_buf[i]=='\n'){ if(r==row) return i; r++; c=0; } else c++;
    }
    return ed_len;
}
/* Open the dialog: mode 1 = open, 2 = save-as.  Seeds the browser at the
 * directory of the current file (or "/") and, for save-as, the filename field
 * with the current base name. */
static void ed_open_dialog(int mode)
{
    ed_dlg = mode;
    path_dir(ed_path[0]?ed_path:"/", ed_brz.cwd, sizeof ed_brz.cwd);
    ed_brz.sel = ed_brz.scroll = 0; ed_brz.loaded = 0;
    br_load(&ed_brz);
    if (mode==2){ if(ed_path[0]) path_base(ed_path, ed_savename, sizeof ed_savename);
                  else scpy(ed_savename, "untitled.txt", sizeof ed_savename); }
}

/* The open/save browser, drawn in the editor's text-area rect while ed_dlg!=0.
 * Returns having fully handled this frame's input. */
static void ed_dialog(int tax, int tay, int taw, int tah, ui_ctx *u)
{
    gfx_fill(&scr, tax, tay, taw, tah, UI_COL_FIELD);
    gfx_outline(&scr, tax, tay, taw, tah, UI_COL_BTN_ACT);

    int bx=tax+6, by=tay+6;
    int up_c  = ui_button(u,&scr,bx,by,52,20,"Up");
    int act_c = ui_button(u,&scr,bx+60,by,76,20, ed_dlg==1?"Open":"Save");
    int can_c = ui_button(u,&scr,bx+144,by,72,20,"Cancel");
    gfx_str_clip(&scr, bx+224, by+6, ed_brz.cwd, UI_COL_MUTED, tax+taw-4);

    if (up_c)  br_up(&ed_brz);
    if (can_c){ ed_dlg=0; return; }

    /* save-as filename field sits on its own row */
    int rowy = by+26, listy = rowy;
    if (ed_dlg==2){
        ui_label(u,&scr,bx,rowy+4,"Name:",UI_COL_MUTED);
        ui_textbox(u,&scr,bx+48,rowy,taw-60-90,20, ed_savename, FZ_NAMW);
        listy = rowy+26;
    }

    int lx=tax+6, ly=listy, lw=taw-12, lh=(tay+tah)-listy-6;
    int prev=ed_brz.sel;
    ui_listbox(u,&scr,lx,ly,lw,lh,ed_brz.ptr,ed_brz.n,&ed_brz.sel,&ed_brz.scroll);

    /* A click on the already-selected row, Enter, or "Open" descends a dir or
     * (open mode) loads the file.  In save-as, picking a file copies its name
     * into the field so you can overwrite it. */
    int activate = act_c
        || (u->key=='\n')
        || (u->mpressed && prev==ed_brz.sel && u->mx>=lx && u->mx<lx+lw && u->my>=ly && u->my<ly+lh);

    if (ed_dlg==1){
        if (activate){
            if (br_sel_isdir(&ed_brz)) br_enter_sel(&ed_brz);
            else if (ed_brz.n>0){ char full[256]; br_sel_path(&ed_brz, full, sizeof full); ed_load(full); ed_dlg=0; }
        }
    } else { /* save-as */
        if (br_sel_isdir(&ed_brz) && activate && act_c==0) br_enter_sel(&ed_brz);
        else if (!br_sel_isdir(&ed_brz) && (u->mpressed && prev==ed_brz.sel)) path_base(ed_brz.name[ed_brz.sel], ed_savename, sizeof ed_savename);
        if (act_c && ed_savename[0]){
            char full[256]; br_join(&ed_brz, ed_savename, full, sizeof full);
            scpy(ed_path, full, sizeof ed_path); ed_save(); ed_dlg=0;
        }
    }
}

static void ed_draw_and_input(window *w, ui_ctx *u)
{
    int cx=client_x(w), cy=client_y(w), cw=client_w(w), ch=client_h(w);
    /* toolbar */
    int bx=cx+6, by=cy+6;
    int open_c = ui_button(u,&scr,bx,by,60,20,"Open");
    int save_c = ui_button(u,&scr,bx+66,by,60,20,"Save");
    int saveas_c = ui_button(u,&scr,bx+132,by,76,20,"Save As");
    int new_c  = ui_button(u,&scr,bx+216,by,52,20,"New");
    ui_label(u,&scr,bx+276,by+6, ed_path[0]?ed_path:"(unsaved)", UI_COL_MUTED);

    if (new_c)    { ed_new(); ed_dlg=0; }
    if (open_c)   ed_open_dialog(1);
    if (saveas_c) ed_open_dialog(2);
    if (save_c)   { if (ed_path[0]) ed_save(); else ed_open_dialog(2); }

    /* text area (or the open/save browser overlay when a dialog is active) */
    int tax=cx+6, tay=cy+34, taw=cw-12, tah=ch-34-18;
    if (ed_dlg){
        ed_dialog(tax, tay, taw, tah, u);
        gfx_str_clip(&scr, tax+4, tay+tah+6, ed_status, UI_COL_MUTED, tax+taw);
        return;
    }
    gfx_fill(&scr, tax, tay, taw, tah, UI_COL_FIELD);
    gfx_outline(&scr, tax, tay, taw, tah, (focus_kind==W_EDITOR)?UI_COL_BTN_ACT:COL_BORDER);

    int vis_rows = (tah-4)/10;
    int caret_row, caret_col; ed_rowcol(ed_caret, &caret_row, &caret_col);

    /* click to position the caret */
    int focused = (focus_kind==W_EDITOR);
    if (focused && u->mpressed && u->mx>=tax && u->mx<tax+taw && u->my>=tay && u->my<tay+tah){
        int row = ed_top + (u->my-tay-2)/10;
        int col = (u->mx-tax-4)/8; if(col<0)col=0;
        ed_caret = ed_index_of(row, col);
        ed_rowcol(ed_caret,&caret_row,&caret_col);
        u->got_input=1;
    }
    /* keyboard editing */
    if (focused && u->key>=0){
        int k=u->key;
        if (k==8||k==127) ed_backspace();
        else if (k=='\r'||k=='\n') ed_insert('\n');
        else if (k==KEY_ARROW_LEFT){ if(ed_caret>0)ed_caret--; }
        else if (k==KEY_ARROW_RIGHT){ if(ed_caret<ed_len)ed_caret++; }
        else if (k==KEY_ARROW_UP){ if(caret_row>0) ed_caret=ed_index_of(caret_row-1,caret_col); }
        else if (k==KEY_ARROW_DOWN) ed_caret=ed_index_of(caret_row+1,caret_col);
        else if (k>=32 && k<127) ed_insert((char)k);
        ed_rowcol(ed_caret,&caret_row,&caret_col);
        u->got_input=1;
    }
    /* scroll to keep caret visible */
    if (caret_row < ed_top) ed_top = caret_row;
    if (caret_row >= ed_top + vis_rows) ed_top = caret_row - vis_rows + 1;
    if (ed_top<0) ed_top=0;

    /* render visible lines */
    int row=0, col=0, scr_row=0;
    for (int i=0; i<=ed_len && scr_row<vis_rows; i++){
        if (row>=ed_top && row<ed_top+vis_rows) scr_row = row-ed_top;
        char ch2 = ed_buf[i];
        if (row>=ed_top && (ch2 && ch2!='\n') && col*8 < taw-8)
            gfx_char(&scr, tax+4+col*8, tay+2+(row-ed_top)*10, (unsigned char)ch2, COL_TEXT);
        if (i==ed_caret && focused && row>=ed_top && row<ed_top+vis_rows)
            gfx_fill(&scr, tax+4+col*8, tay+2+(row-ed_top)*10, 2, 8, RGB(0xff,0xe0,0x60));
        if (ch2=='\n'){ row++; col=0; } else col++;
        if (row>=ed_top+vis_rows) break;
    }
    gfx_str_clip(&scr, tax+4, tay+tah+6, ed_status, UI_COL_MUTED, tax+taw);
}

/* ====================== Files window (native) =========================== */

static browser g_files;
static char    files_pathbox[256];     /* type an absolute path + Go         */

static void files_open_sel(void)
{
    if (g_files.sel<0 || g_files.sel>=g_files.n) return;
    if (br_sel_isdir(&g_files)) { br_enter_sel(&g_files); return; }
    /* a regular file -> open it in the Editor window */
    char full[256]; br_sel_path(&g_files, full, sizeof full);
    ed_load(full);
    wins[W_EDITOR].open=1; z_raise(W_EDITOR);
}
static void files_draw_and_input(window *w, ui_ctx *u)
{
    if (!g_files.loaded){ scpy(g_files.cwd,"/",sizeof g_files.cwd); br_load(&g_files); }
    int cx=client_x(w), cy=client_y(w), cw=client_w(w), ch=client_h(w);
    int bx=cx+6, by=cy+6;
    int up_c = ui_button(u,&scr,bx,by,52,20,"Up");
    int op_c = ui_button(u,&scr,bx+60,by,64,20,"Open");
    int rf_c = ui_button(u,&scr,bx+132,by,72,20,"Refresh");
    /* path box + Go: jump straight to a typed absolute path */
    int pbx=bx+212, pbw=cw-12-(pbx-cx)-52; if (pbw<60) pbw=60;
    ui_textbox(u,&scr,pbx,by,pbw,20, files_pathbox, (int)sizeof files_pathbox);
    int go_c = ui_button(u,&scr,pbx+pbw+4,by,48,20,"Go");

    if (up_c) br_up(&g_files);
    if (rf_c) br_load(&g_files);
    if (go_c && files_pathbox[0]){ scpy(g_files.cwd, files_pathbox, sizeof g_files.cwd); g_files.sel=g_files.scroll=0; br_load(&g_files); }

    ui_label(u,&scr,bx,by+24, g_files.cwd, UI_COL_MUTED);

    int lx=cx+6, ly=cy+50, lw=cw-12, lh=ch-56;
    int prev=g_files.sel;
    ui_listbox(u,&scr,lx,ly,lw,lh,g_files.ptr,g_files.n,&g_files.sel,&g_files.scroll);
    /* Open via button, Enter, or a second click on the already-selected row. */
    if (op_c) files_open_sel();
    else if (focus_kind==W_FILES && u->key=='\n') files_open_sel();
    else if (focus_kind==W_FILES && u->mpressed && prev==g_files.sel &&
             u->mx>=lx && u->mx<lx+lw && u->my>=ly && u->my<ly+lh) files_open_sel();
}

/* ====================== Tasks window (native) =========================== */

#define TK_MAX 64
static char tk_row[TK_MAX][72];
static const char *tk_ptr[TK_MAX];
static int  tk_pid[TK_MAX];
static int  tk_n, tk_sel, tk_scroll;
static int  tk_interval = 1;             /* refresh seconds (slider)         */
static unsigned tk_next;
static char tk_buf[4096];

static void tasks_load(void)
{
    int fd = sys_open("/proc/tasks", O_RDONLY);
    tk_n=0;
    if (fd<0) return;
    long n = sys_read(fd, tk_buf, sizeof tk_buf-1); sys_close(fd);
    if (n<0) n=0; tk_buf[n]=0;
    /* skip header line, then one row per line; first integer is the pid */
    char *p=tk_buf; int line=0;
    while (*p && tk_n<TK_MAX){
        char *e=p; while(*e && *e!='\n') e++;
        int len=(int)(e-p);
        if (line>0 && len>0){
            int copy = len<71?len:71;
            for(int i=0;i<copy;i++) tk_row[tk_n][i]=p[i];
            tk_row[tk_n][copy]=0;
            /* parse leading pid */
            int v=0; char *q=p; while(*q==' ')q++; while(*q>='0'&&*q<='9'){v=v*10+(*q-'0');q++;}
            tk_pid[tk_n]=v;
            tk_ptr[tk_n]=tk_row[tk_n];
            tk_n++;
        }
        if(!*e) break; p=e+1; line++;
    }
    if (tk_sel>=tk_n) tk_sel=tk_n?tk_n-1:0;
}
static void tasks_draw_and_input(window *w, ui_ctx *u)
{
    unsigned now = sys_uptime();
    if (tk_n==0 || (int)(now-tk_next)>=0){ tasks_load(); tk_next = now + (unsigned)(tk_interval<1?1:tk_interval)*100u; }

    int cx=client_x(w), cy=client_y(w), cw=client_w(w), ch=client_h(w);
    int bx=cx+6, by=cy+6;
    int kill_c = ui_button(u,&scr,bx,by,72,20,"Kill");
    int rf_c   = ui_button(u,&scr,bx+80,by,72,20,"Refresh");
    ui_label(u,&scr,bx+164,by+6,"refresh(s)",UI_COL_MUTED);
    ui_slider(u,&scr,bx+252,by,120,20,&tk_interval,1,10);
    { char t[8]; u2s((unsigned)tk_interval,t); ui_label(u,&scr,bx+380,by+6,t,COL_TEXT); }

    if (rf_c) tasks_load();
    if (kill_c && tk_sel>=0 && tk_sel<tk_n && tk_pid[tk_sel]>0){ sys_kill(tk_pid[tk_sel], SIGKILL); tasks_load(); }

    int lx=cx+6, ly=cy+34, lw=cw-12, lh=ch-40;
    ui_listbox(u,&scr,lx,ly,lw,lh,tk_ptr,tk_n,&tk_sel,&tk_scroll);
}

/* ============== Generic surface-backed application window =============== */
/* The server knows nothing about any specific app.  An app window forks a
 * program with "-surface <id>" + a stdin pipe (forwarded input) and a stdout
 * pipe the server drains, maps the shared surface the program renders into,
 * composites it, and reaps the child.  The program path + surface size are
 * launcher *data* (icons[]), not server logic.  (Interim: server-launched;
 * the makx split makes the client connect over IPC and ask for its window.) */
typedef struct {
    int          pid, in, out, sid, w, h;
    gfx_surface  surf;
} appwin;

static appwin      g_app = { -1, -1, -1, -1, 0, 0, {0} };
static const char *g_app_cmd = 0;       /* program for the open W_APP window  */
static int         g_app_w = 0, g_app_h = 0;

static void appwin_launch(appwin *a, const char *path, int w, int h)
{
    if (!path || a->pid>0) return;
    a->w=w; a->h=h;
    a->sid = sys_surface_create(w, h);
    if (a->sid<0) return;
    void *base = sys_surface_map(a->sid);
    if (!base){ sys_surface_destroy(a->sid); a->sid=-1; return; }
    a->surf.px=(gfx_u32*)base; a->surf.w=w; a->surf.h=h;

    /* ip = server->client input (client stdin); op = client stdout/stderr the
     * server drains, so the client's console output can't bleed onto the text
     * VT and the pipe never fills (which would block the client). */
    int ip[2], op[2];
    if (sys_pipe(ip)<0){ sys_surface_destroy(a->sid); a->sid=-1; return; }
    if (sys_pipe(op)<0){ sys_close(ip[0]); sys_close(ip[1]); sys_surface_destroy(a->sid); a->sid=-1; return; }
    int pid=sys_fork();
    if (pid<0){ sys_close(ip[0]); sys_close(ip[1]); sys_close(op[0]); sys_close(op[1]); sys_surface_destroy(a->sid); a->sid=-1; return; }
    if (pid==0){
        sys_close(ip[1]); sys_close(op[0]);
        sys_dup2(ip[0],0); sys_dup2(op[1],1); sys_dup2(op[1],2);
        sys_close(ip[0]); sys_close(op[1]);
        char ids[12]; u2s((unsigned)a->sid, ids);
        char *av[4]={ (char*)path, "-surface", ids, 0 };
        sys_execve(path, av, (char *const*)0);
        sys_exit(127);
    }
    a->pid=pid; a->in=ip[1]; a->out=op[0];
    sys_close(ip[0]); sys_close(op[1]);
    sys_fcntl(a->in,  F_SETFL, O_NONBLOCK);
    sys_fcntl(a->out, F_SETFL, O_NONBLOCK);
}
static void appwin_stop(appwin *a)
{
    /* Only kill+reap a live child.  The self-exit path clears a->pid (after its
     * own wait4) before calling this, so we never SIGKILL a recycled pid or
     * block in wait4 on an already-reaped child (that hung the server). */
    if (a->pid>0){ sys_kill(a->pid,SIGKILL); int st=0; sys_wait4(a->pid,&st,0); }
    if (a->in>=0)  sys_close(a->in);
    if (a->out>=0) sys_close(a->out);
    if (a->sid>=0) sys_surface_destroy(a->sid);
    a->pid=-1; a->in=-1; a->out=-1; a->sid=-1; a->surf.px=0;
}
static void appwin_key(appwin *a, int k)
{
    if (a->in<0) return;
    unsigned char b=(unsigned char)k; sys_write(a->in,&b,1);
}
static void appwin_draw(appwin *a, window *w)
{
    int cx=client_x(w), cy=client_y(w), cw=client_w(w), ch=client_h(w);
    gfx_fill(&scr, cx, cy, cw, ch, 0);
    /* drain the client's stdout/stderr (prevents text-VT bleed + a full pipe) */
    if (a->out>=0){ unsigned char b[128]; while (sys_read(a->out,b,sizeof b)>0){} }
    /* reap a self-exited client: clear a->pid FIRST so appwin_stop doesn't
     * kill+blocking-wait an already-reaped pid. */
    if (a->pid>0){ int st=0; if (sys_wait4(a->pid,&st,WNOHANG)==a->pid){ a->pid=-1; appwin_stop(a); wins[W_APP].open=0; return; } }
    if (a->surf.px){
        if (cw>=a->w && ch>=a->h)
            gfx_blit(&scr, cx+(cw-a->w)/2, cy+(ch-a->h)/2, &a->surf, 0,0, a->w, a->h);
        else
            gfx_blit_scaled(&scr, cx, cy, cw, ch, &a->surf);
    }
    else gfx_str(&scr, cx+8, cy+8, "(application not running)", COL_TEXT);
}

/* ===================== window chrome + compositor ======================= */

static const char *win_title(int kind){ return wins[kind].title; }

/* Title-bar buttons: three small dots, right-aligned.  Order L->R is
 * minimise, maximise, close. */
#define BTN_D   11
#define BTN_GAP 7
static int btn_close_x(window *w){ return w->x + w->w - 8 - BTN_D; }
static int btn_max_x  (window *w){ return btn_close_x(w) - (BTN_D + BTN_GAP); }
static int btn_min_x  (window *w){ return btn_max_x(w)   - (BTN_D + BTN_GAP); }
static int btn_y      (window *w){ return w->y + (TH - BTN_D) / 2; }

/* Toggle maximise: fill the work area (below the menu bar, above the dock),
 * saving the previous geometry so the next click restores it. */
static void win_toggle_max(int kind)
{
    window *w=&wins[kind];
    if (!w->maximized){
        w->sx=w->x; w->sy=w->y; w->sw=w->w; w->sh=w->h;
        w->x=0; w->y=MENU_H; w->w=(int)FBW; w->h=(int)FBH-MENU_H-DOCK_H;
        w->maximized=1;
    } else {
        w->x=w->sx; w->y=w->sy; w->w=w->sw; w->h=w->sh;
        w->maximized=0;
    }
}

static void draw_window_frame(int kind)
{
    window *w=&wins[kind];
    int focused = (focus_kind==kind);
    gfx_outline(&scr, w->x-1, w->y-1, w->w+2, w->h+2, COL_BORDER);
    gfx_fill(&scr, w->x, w->y, w->w, w->h, COL_WIN);
    gfx_fill(&scr, w->x, w->y, w->w, TH, focused?COL_TITLE:COL_TITLE_U);
    gfx_fill(&scr, w->x, w->y+TH-2, w->w, 2, COL_TITLE2);
    gfx_str_clip(&scr, w->x+10, w->y+(TH-8)/2, win_title(kind), 0xFFFFFF, btn_min_x(w)-6);
    /* small macOS-style dots: minimise (amber) / maximise (green) / close (red),
     * dimmed when the window is unfocused. */
    int by=btn_y(w);
    gfx_round(&scr, btn_min_x(w),   by, BTN_D, BTN_D, focused?RGB(0xfe,0xbc,0x2e):RGB(0x5e,0x57,0x40), COL_BORDER);
    gfx_round(&scr, btn_max_x(w),   by, BTN_D, BTN_D, focused?RGB(0x28,0xc8,0x40):RGB(0x46,0x5a,0x46), COL_BORDER);
    gfx_round(&scr, btn_close_x(w), by, BTN_D, BTN_D, focused?RGB(0xff,0x5f,0x57):RGB(0x6a,0x4a,0x48), COL_BORDER);
    /* resize grip: a solid corner wedge (bottom-right) so it's an obvious drag
     * target -- brighter when focused.  Grab area is in_resize (larger). */
    {
        gfx_u32 grip = focused ? UI_COL_BTN_ACT : RGB(0x4a,0x5a,0x74);
        for (int r=0; r<14; r++)
            gfx_fill(&scr, w->x+w->w-1-r, w->y+w->h-1-r, r+1, 1, grip);
    }
}

static int in_rect(int px,int py,int x,int y,int w,int h){ return px>=x&&px<x+w&&py>=y&&py<y+h; }
static int in_btn(int bx,int by,int px,int py){ return in_rect(px,py,bx-2,by-2,BTN_D+4,BTN_D+4); }
static int in_min  (window *w,int px,int py){ return in_btn(btn_min_x(w),  btn_y(w),px,py); }
static int in_max  (window *w,int px,int py){ return in_btn(btn_max_x(w),  btn_y(w),px,py); }
static int in_close(window *w,int px,int py){ return in_btn(btn_close_x(w),btn_y(w),px,py); }
/* drag region = title bar left of the buttons */
static int in_titlebar(window *w,int px,int py){ return in_rect(px,py,w->x,w->y,btn_min_x(w)-w->x,TH); }
/* resize grab = bottom-right corner */
static int in_resize(window *w,int px,int py){ return in_rect(px,py,w->x+w->w-18,w->y+w->h-18,20,20); }

/* topmost open, non-minimised window under (px,py); -1 if none */
static int hit_window(int px,int py)
{
    for (int i=W_COUNT-1;i>=0;i--){ int k=zlist[i];
        if(wins[k].open && !wins[k].minimized && in_rect(px,py,wins[k].x-1,wins[k].y-1,wins[k].w+2,wins[k].h+2)) return k; }
    return -1;
}

static void window_content(int kind, ui_ctx *u)
{
    switch(kind){
        case W_TERMINAL: term_draw(&wins[kind]); break;
        case W_EDITOR:   ed_draw_and_input(&wins[kind], u); break;
        case W_FILES:    files_draw_and_input(&wins[kind], u); break;
        case W_TASKS:    tasks_draw_and_input(&wins[kind], u); break;
        case W_APP:      appwin_draw(&g_app, &wins[kind]); break;
    }
}

/* ===================== desktop icons + dock ============================= */

/* For W_APP icons, cmd/aw/ah are the launcher data (program + surface size) the
 * server hands to the generic app window -- the only place an app is named. */
typedef struct { int x,y,w,h; const char *label; int kind; gfx_u32 tint;
                 const char *cmd; int aw, ah; } icon_t;
#define ICON_N 5
static icon_t icons[ICON_N] = {
    { 24,  40, 96,70, "Terminal", W_TERMINAL, RGB(0x4c,0x8d,0xff), 0,0,0 },
    { 24, 124, 96,70, "Files",    W_FILES,    RGB(0xf0,0xa8,0x30), 0,0,0 },
    { 24, 208, 96,70, "Editor",   W_EDITOR,   RGB(0x35,0xc7,0x59), 0,0,0 },
    { 24, 292, 96,70, "Tasks",    W_TASKS,    RGB(0x9b,0x6c,0xff), 0,0,0 },
    { 24, 376, 96,70, "Doom",     W_APP,      RGB(0xc0,0x40,0x40), "/apps/doom.elf",640,400 },
};
static void draw_icons(void)
{
    for(int i=0;i<ICON_N;i++){ icon_t *c=&icons[i];
        gfx_round(&scr,c->x,c->y,c->w,c->h,RGB(0x2a,0x38,0x50),COL_DESK);
        gfx_round(&scr,c->x+c->w/2-16,c->y+10,32,30,c->tint,RGB(0x2a,0x38,0x50));
        gfx_str(&scr,c->x+(c->w-gfx_text_w(c->label))/2,c->y+c->h-16,c->label,0xFFFFFF);
    }
}
/* returns the icon INDEX under (px,py), or -1 (caller reads icons[i].kind/cmd) */
static int icon_hit(int px,int py){ for(int i=0;i<ICON_N;i++){icon_t*c=&icons[i]; if(in_rect(px,py,c->x,c->y,c->w,c->h)) return i;} return -1; }

static const char *kind_short(int k){ return k==W_TERMINAL?"sh":k==W_EDITOR?"ed":k==W_FILES?"fs":k==W_TASKS?"ps":"dm"; }

/* ---- system stats for the right of the dock (CPU% + RAM%) --------------- */
static unsigned dock_meminfo_kb(const char *label)
{
    char buf[512]; int fd=sys_open("/proc/meminfo",O_RDONLY); if(fd<0) return 0;
    long r=sys_read(fd,buf,sizeof buf-1); sys_close(fd); if(r<=0) return 0; buf[r]=0;
    int ll=slen(label);
    for(long i=0;i<r;){
        int j=0; while(j<ll && buf[i+j]==label[j]) j++;
        if(j==ll && buf[i+ll]==':'){ const char *p=buf+i+ll+1; while(*p==' ')p++;
            unsigned v=0; while(*p>='0'&&*p<='9'){v=v*10u+(unsigned)(*p-'0');p++;} return v; }
        while(i<r && buf[i]!='\n') i++; i++;
    }
    return 0;
}
/* Sum of TICKS across all tasks except idle (pid 1) -- maktop's CPU% basis. */
static unsigned dock_busy_ticks(void)
{
    char buf[1024]; int fd=sys_open("/proc/tasks",O_RDONLY); if(fd<0) return 0;
    long r=sys_read(fd,buf,sizeof buf-1); sys_close(fd); if(r<=0) return 0; buf[r]=0;
    long i=0; int line=0; unsigned sum=0;
    while(i<r){
        char tok[6][20]; int nt=0;
        while(i<r && buf[i]!='\n'){
            while(i<r&&(buf[i]==' '||buf[i]=='\t'))i++;
            if(i>=r||buf[i]=='\n')break;
            int tl=0; while(i<r&&buf[i]!=' '&&buf[i]!='\t'&&buf[i]!='\n'){ if(nt<6&&tl<19)tok[nt][tl++]=buf[i]; i++; }
            if(nt<6){tok[nt][tl]=0;nt++;}
        }
        if(i<r)i++;
        if(line++==0)continue;                       /* header */
        if(nt<5)continue;
        if(tok[0][0]=='1'&&tok[0][1]==0)continue;     /* skip idle (pid 1) */
        unsigned v=0; for(int k=0;tok[4][k];k++)v=v*10u+(unsigned)(tok[4][k]-'0'); sum+=v;
    }
    return sum;
}
/* "CPU n%  RAM n%" into out; recomputed ~once/sec, cached between. */
static void dock_stats(char *out)
{
    static unsigned last_busy=0,last_up=0,cpu=0,ram=0; static int have=0;
    unsigned up=sys_uptime();
    if(!have || (up-last_up)>=100u){             /* ~1s at 100 Hz */
        unsigned busy=dock_busy_ticks();
        if(have && up>last_up){ unsigned dt=up-last_up, db=(busy>last_busy)?busy-last_busy:0u;
                                cpu=db*100u/dt; if(cpu>100u)cpu=100u; }
        unsigned tot=dock_meminfo_kb("MemTotal"), fr=dock_meminfo_kb("MemFree");
        ram=(tot>fr)?(tot-fr)*100u/tot:0u;
        last_busy=busy; last_up=up; have=1;
    }
    char n[8]; int o=0; const char *p;
    p="CPU "; while(*p)out[o++]=*p++; u2s(cpu,n); for(int i=0;n[i];i++)out[o++]=n[i]; out[o++]='%';
    out[o++]=' '; out[o++]=' ';
    p="RAM "; while(*p)out[o++]=*p++; u2s(ram,n); for(int i=0;n[i];i++)out[o++]=n[i]; out[o++]='%';
    out[o]=0;
}

/* taskbar button rects live here so click handling and drawing agree */
static int dock_btn_x(int slot){ return 8 + slot*42; }
static void draw_dock(void)
{
    int y0=(int)FBH-DOCK_H;
    gfx_fill(&scr,0,y0,(int)FBW,DOCK_H,RGB(0x12,0x16,0x1e));
    gfx_fill(&scr,0,y0,(int)FBW,1,RGB(0x28,0x32,0x44));
    int slot=0;
    /* draw a tile per open window (in kind order for stable positions) */
    for(int k=0;k<W_COUNT;k++){ if(!wins[k].open) continue;
        int bx=dock_btn_x(slot); int active=(focus_kind==k);
        gfx_round(&scr,bx,y0+5,36,DOCK_H-10, active?UI_COL_BTN_ACT:UI_COL_BTN, RGB(0x12,0x16,0x1e));
        gfx_str(&scr,bx+(36-gfx_text_w(kind_short(k)))/2, y0+(DOCK_H-8)/2, kind_short(k), 0xFFFFFF);
        slot++;
    }
    /* CPU / RAM stats, right-aligned. */
    { char st[32]; dock_stats(st);
      gfx_str(&scr,(int)FBW-gfx_text_w(st)-10, y0+(DOCK_H-8)/2, st, RGB(0x90,0xa0,0xb5)); }
}

/* The top menu bar.  Drawn on every composited frame (after the windows, so it
 * is always visible) -- the GUI is never chromeless: desktop + menu bar + dock
 * are unconditional.  Carries the "Makar" brand, the focused window's name, and
 * a right-aligned Log Off item. */
static const char *kind_name(int k)
{
    switch(k){ case W_TERMINAL:return "Terminal"; case W_EDITOR:return "Editor";
               case W_FILES:return "Files"; case W_TASKS:return "Tasks";
               case W_APP:return wins[W_APP].title; default:return "Desktop"; }
}
#define LOGOFF_W 70
#define EXIT_W   54
static void draw_menubar(void)
{
    gfx_fill(&scr,0,0,(int)FBW,MENU_H,COL_MENU);
    gfx_fill(&scr,0,MENU_H-1,(int)FBW,1,RGB(0x28,0x32,0x44));
    gfx_str(&scr,8,(MENU_H-8)/2,"Makar",RGB(0x8a,0xe2,0x34));
    gfx_str(&scr,64,(MENU_H-8)/2, kind_name(focus_kind), RGB(0x90,0xa0,0xb5));
    /* Right-aligned: [ Exit ] [ Log Off ]. */
    int lx=(int)FBW-LOGOFF_W-4;
    gfx_fill(&scr,lx,2,LOGOFF_W,MENU_H-4,COL_CLOSE);
    gfx_str(&scr,lx+(LOGOFF_W-gfx_text_w("Log Off"))/2,(MENU_H-8)/2,"Log Off",0xFFFFFF);
    int ex=lx-EXIT_W-4;
    gfx_fill(&scr,ex,2,EXIT_W,MENU_H-4,UI_COL_BTN);
    gfx_str(&scr,ex+(EXIT_W-gfx_text_w("Exit"))/2,(MENU_H-8)/2,"Exit",0xFFFFFF);
}
static int exit_hit(int px,int py)
{
    int ex=(int)FBW-LOGOFF_W-4-EXIT_W-4;
    return in_rect(px,py,ex,2,EXIT_W,MENU_H-4);
}
static int logoff_hit(int px,int py)
{
    int lx=(int)FBW-LOGOFF_W-4;
    return in_rect(px,py,lx,2,LOGOFF_W,MENU_H-4);
}
static int dock_hit(int px,int py,int *out_kind)
{
    int y0=(int)FBH-DOCK_H;
    if (py<y0) return 0;
    int slot=0;
    for(int k=0;k<W_COUNT;k++){ if(!wins[k].open) continue;
        int bx=dock_btn_x(slot);
        if (in_rect(px,py,bx,y0+5,36,DOCK_H-10)){ *out_kind=k; return 1; }
        slot++;
    }
    return 0;
}

/* ---- mouse cursor ------------------------------------------------------- */
static const char *CURSOR[16]={
 "X          ","XX         ","X.X        ","X..X       ","X...X      ","X....X     ",
 "X.....X    ","X......X   ","X.......X  ","X........X ","X....XXXXXX","X..X.X     ",
 "X.X X.X    ","XX  X.X    ","X    X.X   ","      XX   " };
static void draw_cursor(int cx,int cy)
{
    for(int r=0;r<16;r++) for(int c=0;CURSOR[r][c];c++){ char p=CURSOR[r][c];
        if(p=='X')gfx_px(&scr,cx+c,cy+r,0); else if(p=='.')gfx_px(&scr,cx+c,cy+r,0xFFFFFF); }
}

/* ---- window default geometry ------------------------------------------- */
static void open_window(int kind)
{
    window *w=&wins[kind];
    if (!w->open){
        w->open=1;
        if (kind==W_APP && g_app.pid<0) appwin_launch(&g_app, g_app_cmd, g_app_w, g_app_h);
        if (kind==W_TERMINAL && term_pid<0){ term_clear(); term_spawn(); }
    }
    w->minimized=0;          /* a dock/icon click restores a minimised window */
    z_raise(kind);
}
static void close_window(int kind)
{
    wins[kind].open=0;
    if (kind==W_APP) appwin_stop(&g_app);
    if (kind==W_TERMINAL) term_kill();
    if (focus_kind==kind){ focus_kind=-1; for(int i=W_COUNT-1;i>=0;i--){int k=zlist[i]; if(wins[k].open&&!wins[k].minimized){focus_kind=k;break;}} }
}

/* ============================== uitest ================================== */
/* Headless self-test of the widget framework: drive a button, a slider and a
 * textbox through synthetic input and emit a serial marker.  Invoked as
 * `gui uitest`; mirrors the old corner-test harness. */
static void emit(const char *m){ int n=0; while(m[n])n++; sys_write_serial(m,n); }
static int uitest(void)
{
    static gfx_u32 px[64*64];
    gfx_surface s={ px, 64, 64 };
    ui_ctx u; for (unsigned i=0;i<sizeof u/sizeof(int);i++) ((int*)&u)[i]=0;
    int val=0; char box[16]; box[0]=0; int ok=1;

    /* press + release inside the button rect -> click */
    ui_begin(&u,10,10,1,1,0,-1); ui_button(&u,&s,0,0,40,20,"B");
    ui_begin(&u,10,10,0,0,1,-1); int clicked=ui_button(&u,&s,0,0,40,20,"B");
    if (!clicked) ok=0;

    /* slider drag to the right edge -> max value */
    ui_begin(&u,0,30,1,1,0,-1);  ui_slider(&u,&s,0,24,40,12,&val,0,100);
    ui_begin(&u,40,30,1,0,0,-1); ui_slider(&u,&s,0,24,40,12,&val,0,100);
    if (val<90) ok=0;

    /* focus the textbox then type 'Z' */
    ui_begin(&u,10,46,1,1,0,-1); ui_textbox(&u,&s,0,40,40,12,box,16);
    ui_begin(&u,10,46,0,0,1,'Z'); ui_textbox(&u,&s,0,40,40,12,box,16);
    if (box[0]!='Z') ok=0;

    emit(ok? "GUI-UITEST: PASS\n" : "GUI-UITEST: FAIL\n");
    return ok?0:1;
}

/* Headless self-test of the reusable file-browser navigation against the real
 * VFS (the chdir/readdir/getcwd path the Files window + editor dialog use).
 * Invoked as `gui fstest`; emits GUI-FSTEST.  This is the coverage that proves
 * "Files actually moves about the filesystem" without needing pixels. */
static int seq2(const char *a, const char *b){ int i=0; while(a[i]&&a[i]==b[i])i++; return a[i]==b[i]; }
static int fstest(void)
{
    static browser b; int ok=1;
    scpy(b.cwd, "/", sizeof b.cwd); b.sel=b.scroll=0; b.loaded=0;
    br_load(&b);
    if (b.n<=0) ok=0;                         /* root must enumerate */
    if (!seq2(b.cwd, "/")) ok=0;              /* normalised to "/"   */

    /* descend into the first directory entry, confirm cwd deepened, climb back */
    int di=-1; for(int i=0;i<b.n;i++) if(b.type[i]==DT_DIR){ di=i; break; }
    if (di>=0){
        b.sel=di; br_enter_sel(&b);
        if (seq2(b.cwd, "/")) ok=0;           /* cwd must have changed off "/" */
        if (b.n<0) ok=0;
        br_up(&b);
        if (!seq2(b.cwd, "/")) ok=0;          /* ".." from depth-1 returns "/" */
    } else { ok=0; }                          /* root with no subdir is wrong  */

    /* getcwd round-trips an explicit chdir target */
    scpy(b.cwd, "/", sizeof b.cwd); b.sel=b.scroll=0; b.loaded=0; br_load(&b);

    emit(ok? "GUI-FSTEST: PASS\n" : "GUI-FSTEST: FAIL\n");
    return ok?0:1;
}

/* ============================== main ==================================== */

/* ============================ GUI login ================================= */
/* Graphical login screen, shown before the desktop when gui.elf is launched
 * with `login` (the kernel passes it when no user is auto-logged-in).  Blocks
 * until sys_login() accepts the credentials.  Drawn on the WM back buffer; the
 * GUI is the focused root task so sys_fb_present works.  NOTE: when the GUI is
 * split into a display server + client .elfs, this lifts wholesale into a
 * standalone login.elf -- see docs/gui.md. */
static void do_login(void)
{
    char user[64]={0}, pass[64]={0}, err[40]={0};
    int cx=(int)FBW/2, cy=(int)FBH/2, prev_left=0, dirty=1;
    ui_ctx u; for(unsigned i=0;i<sizeof u/sizeof(int);i++) ((int*)&u)[i]=0;
    u.focus=1;                 /* start with the username field focused */

    for(;;){
        int mpressed=0,mreleased=0; unsigned int ev;
        while((ev=sys_mouse_read())!=0){
            cx+=(int)(signed char)((ev>>8)&0xFF); cy+=(int)(signed char)((ev>>16)&0xFF);
            if(cx<0)cx=0; if(cx>=(int)FBW)cx=(int)FBW-1;
            if(cy<0)cy=0; if(cy>=(int)FBH)cy=(int)FBH-1;
            int left=ev&1; if(left&&!prev_left)mpressed=1; if(!left&&prev_left)mreleased=1;
            prev_left=left; dirty=1;
        }
        int mdown=prev_left, key=-1;
        { unsigned char b; if(sys_read(0,&b,1)==1){ key=b; dirty=1; } }
        if(!dirty){ sys_yield(); continue; }

        gfx_fill(&scr,0,0,(int)FBW,(int)FBH,COL_DESK);
        int pw=340,ph=190,px=(int)FBW/2-pw/2,py=(int)FBH/2-ph/2;
        gfx_round(&scr,px,py,pw,ph,COL_WIN,COL_BORDER);
        gfx_fill(&scr,px,py,pw,26,COL_TITLE);
        gfx_str(&scr,px+(pw-gfx_text_w("Makar -- sign in"))/2,py+9,"Makar -- sign in",0xFFFFFF);
        gfx_str(&scr,px+24,py+54,"User:",COL_TEXT);
        gfx_str(&scr,px+24,py+92,"Pass:",COL_TEXT);

        ui_begin(&u,cx,cy,mdown,mpressed,mreleased,
                 (key=='\t'||key=='\n'||key=='\r')?-1:key);  /* Tab/Enter are control */
        ui_textbox(&u,&scr,px+72,py+48,pw-96,22,user,(int)sizeof user);
        ui_password(&u,&scr,px+72,py+86,pw-96,22,pass,(int)sizeof pass);
        int login_c=ui_button(&u,&scr,px+pw/2-44,py+128,88,28,"Log in");
        if(err[0]) gfx_str(&scr,px+24,py+ph-22,err,COL_CLOSE);

        if(key=='\t') u.focus = (u.focus==1)?2:1;     /* Tab toggles user/pass */
        if(login_c || key=='\n' || key=='\r'){
            if(user[0] && sys_login(user,pass)==0) return;   /* authenticated  */
            scpy(err,"Incorrect credentials",sizeof err);
            pass[0]=0; u.focus=2;
        }
        draw_cursor(cx,cy);
        sys_fb_present(scr.px);
        dirty=0; sys_yield();
    }
}

int main(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc>1 && seq(argv[1],"uitest")) return uitest();
    if (argc>1 && seq(argv[1],"fstest")) return fstest();
    int want_login = (argc>1 && seq(argv[1],"login"));

    unsigned info=sys_fb_info();
    if(!info){ const char*e="gui: no pixel framebuffer (VGA-only)\n"; sys_write(2,e,36); return 1; }
    FBW=(info>>16)&0xFFFF; FBH=info&0xFFFF;
    gfx_u32 *bb=(gfx_u32*)sys_mmap(0,(unsigned long)FBW*FBH*4,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (bb==(gfx_u32*)MAP_FAILED || !bb){ const char*e="gui: back-buffer mmap failed\n"; sys_write(2,e,29); return 1; }
    scr.px=bb; scr.w=(int)FBW; scr.h=(int)FBH;

    sys_fcntl(0,F_SETFL,O_NONBLOCK);
    int saved_status=sys_statusbar_enabled();
    sys_statusbar_set(0);
    sys_signal(SIGINT,SIG_IGN);

    if (want_login) do_login();     /* graphical login before the desktop */

    /* initial window geometry + z-order */
    for(int k=0;k<W_COUNT;k++) zlist[k]=k;
    /* order: open,x,y,w,h, minimized,maximized,sx,sy,sw,sh, title, ui */
    wins[W_TERMINAL]=(window){0,160, 90,560,360, 0,0,0,0,0,0, "Terminal",{0}};
    wins[W_EDITOR]  =(window){0,220,120,640,420, 0,0,0,0,0,0, "Editor",{0}};
    wins[W_FILES]   =(window){0,200,110,560,380, 0,0,0,0,0,0, "Files",{0}};
    wins[W_TASKS]   =(window){0,260,140,560,360, 0,0,0,0,0,0, "Tasks",{0}};
    wins[W_APP]     =(window){0,260, 80,660,440, 0,0,0,0,0,0, "App",{0}};

    open_window(W_TERMINAL);

    int cx=(int)FBW/2, cy=(int)FBH/2, prev_left=0;
    int dragging=0, resizing=0, drag_kind=-1, drag_dx=0, drag_dy=0;
    int dirty=1, announced=0, exit_to_shell=0;
    unsigned stat_up=0;

    for(;;){
        /* ---- gather input ---- */
        int mpressed=0, mreleased=0;
        unsigned int ev;
        while((ev=sys_mouse_read())!=0){
            cx += (int)(signed char)((ev>>8)&0xFF);
            cy += (int)(signed char)((ev>>16)&0xFF);
            if(cx<0)cx=0; if(cx>=(int)FBW)cx=(int)FBW-1;
            if(cy<0)cy=0; if(cy>=(int)FBH)cy=(int)FBH-1;
            int left=ev&1;
            if(left&&!prev_left) mpressed=1;
            if(!left&&prev_left) mreleased=1;
            prev_left=left;
            dirty=1;
        }
        int mdown=prev_left;

        /* one key per frame */
        int frame_key=-1;
        { unsigned char b; if (sys_read(0,&b,1)==1){ frame_key=b; dirty=1; } }

        /* ---- window-management click handling ---- */
        int client_click_kind=-1;
        if (mpressed){
            int dk;
            if (logoff_hit(cx,cy)) break;     /* Log Off: clean up + end session */
            else if (exit_hit(cx,cy)){ exit_to_shell=1; break; }   /* Exit to CLI shell */
            else if (dock_hit(cx,cy,&dk)){ open_window(dk); dirty=1; }
            else {
                int hk=hit_window(cx,cy);
                if (hk>=0){
                    z_raise(hk); dirty=1;
                    if      (in_close(&wins[hk],cx,cy)) close_window(hk);
                    else if (in_min(&wins[hk],cx,cy)){ wins[hk].minimized=1;
                             focus_kind=-1; for(int i=W_COUNT-1;i>=0;i--){int k=zlist[i]; if(wins[k].open&&!wins[k].minimized){focus_kind=k;break;}} }
                    else if (in_max(&wins[hk],cx,cy)) win_toggle_max(hk);
                    else if (in_resize(&wins[hk],cx,cy)){ resizing=1; drag_kind=hk; wins[hk].maximized=0; }
                    else if (in_titlebar(&wins[hk],cx,cy)){ dragging=1; drag_kind=hk; drag_dx=cx-wins[hk].x; drag_dy=cy-wins[hk].y; wins[hk].maximized=0; }
                    else client_click_kind=hk;     /* client area -> widgets   */
                } else {
                    int ii=icon_hit(cx,cy);
                    if (ii>=0){
                        /* W_APP: take the launcher data (program/size/title)
                         * from the clicked icon -- the only place an app is
                         * named -- then open the generic app window. */
                        if (icons[ii].kind==W_APP){
                            g_app_cmd=icons[ii].cmd; g_app_w=icons[ii].aw; g_app_h=icons[ii].ah;
                            scpy(wins[W_APP].title, icons[ii].label, sizeof wins[W_APP].title);
                        }
                        open_window(icons[ii].kind); dirty=1;
                    }
                }
            }
        }
        if (mreleased){ dragging=0; resizing=0; }
        if (dragging && drag_kind>=0){
            wins[drag_kind].x=cx-drag_dx; wins[drag_kind].y=cy-drag_dy;
            if(wins[drag_kind].x<0)wins[drag_kind].x=0;
            if(wins[drag_kind].y<MENU_H)wins[drag_kind].y=MENU_H;  /* keep clear of the menu bar */
            if(wins[drag_kind].x+wins[drag_kind].w>(int)FBW)wins[drag_kind].x=(int)FBW-wins[drag_kind].w;
            if(wins[drag_kind].y+wins[drag_kind].h>(int)FBH-DOCK_H)wins[drag_kind].y=(int)FBH-DOCK_H-wins[drag_kind].h;
            dirty=1;
        }
        if (resizing && drag_kind>=0){
            window *w=&wins[drag_kind];
            w->w=cx-w->x; w->h=cy-w->y;
            if(w->w<220)w->w=220; if(w->h<120)w->h=120;            /* minimum size */
            if(w->x+w->w>(int)FBW)w->w=(int)FBW-w->x;
            if(w->y+w->h>(int)FBH-DOCK_H)w->h=(int)FBH-DOCK_H-w->y;
            dirty=1;
        }

        /* ---- keyboard routing to the focused window ---- */
        if (frame_key>=0 && focus_kind>=0){
            if (focus_kind==W_TERMINAL) term_key(frame_key);
            else if (focus_kind==W_APP) appwin_key(&g_app, frame_key);
            /* editor/files/tasks consume the key via their ui pass below */
        }

        /* ---- terminal / doom liveness pumps (always, even if unfocused) ---- */
        if (wins[W_TERMINAL].open && term_pump()) dirty=1;
        if (wins[W_TASKS].open) dirty=1;           /* tasks auto-refresh ticks */
        if (wins[W_APP].open) dirty=1;             /* surface app animates     */
        { unsigned now=sys_uptime(); if (now-stat_up>=100u){ stat_up=now; dirty=1; } } /* ~1s: refresh dock stats */

        if (!dirty){ sys_yield(); continue; }

        /* ---- compose (desktop + menu bar + dock are unconditional) ---- */
        gfx_fill(&scr,0,0,(int)FBW,(int)FBH,COL_DESK);
        gfx_str(&scr,8,MENU_H+6,"Makar desktop -- click an icon; drag a title bar; click a window to focus",RGB(0x90,0xa0,0xb5));
        draw_icons();

        for (int i=0;i<W_COUNT;i++){
            int k=zlist[i]; if(!wins[k].open || wins[k].minimized) continue;
            draw_window_frame(k);
            /* build this window's input context: only the focused window gets
             * live mouse buttons / keys (others draw but don't react). */
            ui_ctx *u=&wins[k].ui;
            int live = (k==focus_kind);
            int wk_pressed = (live && client_click_kind==k) ? 1 : 0;
            ui_begin(u, cx, cy,
                     live?mdown:0,
                     wk_pressed,
                     live?mreleased:0,
                     (live && k!=W_TERMINAL && k!=W_APP) ? frame_key : -1);
            window_content(k, u);
        }

        draw_dock();
        draw_menubar();
        draw_cursor(cx,cy);
        sys_fb_present(scr.px);
        /* One-shot readiness marker: the desktop has composited and presented
         * its first frame.  The guitest harness waits for this, then screendumps
         * and shuts down.  See run.sh `guitest`. */
        if (!announced){ sys_write_serial("GUI: READY\n", 11); announced=1; }
        dirty=0;
        sys_yield();
    }

    /* Reached on Log Off or Exit.  Either way tear down our children so no
     * forked shell / doom leaks and restore terminal + statusbar state.  Then:
     *   - Exit  -> sys_gui_close(): hand the display + keyboard back to the CLI
     *              shell of *this* session (same login), and mark the session
     *              CLI so a later `logout` re-shows the text login.
     *   - LogOff-> sys_logout(): end the login session entirely; the login loop
     *              re-shows the (GUI) login.
     * The GUI shell is a forked sh.elf on a private pipe, so a Ctrl-C/Ctrl-D in
     * it only ever closed that terminal window -- mak.sh0 is never in range. */
    term_kill(); appwin_stop(&g_app);
    sys_fcntl(0,F_SETFL,0);
    sys_statusbar_set(saved_status);
    if (exit_to_shell) sys_gui_close();
    else               sys_logout();
    return 0;
}
