/*
 * mxedit.elf -- text editor, as a makx client.  A multi-line buffer with a
 * toolbar (Open/Save/Save As/New) and an open/save dialog built on the shared
 * gui_browser model, drawn with gui_ui into its makx surface.  Was the W_EDITOR
 * window kind in the old monolithic wm.c.  `-open <path>` preloads a file.
 */
#include "syscall.h"
#include <stdlib.h>   /* malloc/free (libc.a) -- undo/redo snapshots */
#include <string.h>   /* memcpy */
#include "gui_gfx.h"
#include "gui_ui.h"
#include "gui_browser.h"
#include "makx.h"

#define RGB GFX_RGB
#define COL_TEXT RGB(0xd3,0xd7,0xcf)

static void scpy(char *d,const char *s,int max){int i=0;while(s[i]&&i<max-1){d[i]=s[i];i++;}d[i]=0;}
static char *scat(char *d,const char *s){while(*d)d++;while(*s)*d++=*s++;*d=0;return d;}
static int  streq(const char *a,const char *b){int i=0;while(a[i]&&a[i]==b[i])i++;return a[i]==b[i];}

#define ED_MAX (32*1024)
static char ed_buf[ED_MAX];
static char ed_path[128];
static int  ed_len, ed_caret, ed_top, ed_dirty_flag;
static char ed_status[80];

static int     ed_dlg;          /* 0 none, 1 open, 2 save-as */
static browser ed_brz;
static char    ed_savename[BR_NAMW];

/* ---- selection, clipboard, undo/redo (T52) ------------------------------ */
static int  ed_sel = -1;        /* selection anchor index, -1 = no selection */
static int  ed_menu = -1;       /* open top menu (-1 none) */
static int  ed_about = 0;       /* About modal shown */
static int  ed_ctx = 0;         /* right-click context menu shown */
static int  ed_ctx_x, ed_ctx_y; /* where it was opened */

#define UNDO_MAX 32
typedef struct { char *buf; int len, caret; } ed_snap;
static ed_snap ud[UNDO_MAX], rd[UNDO_MAX];
static int ud_n, rd_n;

static void snap_clear(ed_snap *st, int *n){ while(*n>0){ (*n)--; free(st[*n].buf); st[*n].buf=0; } }
static void snap_push(ed_snap *st, int *n){          /* push current buffer */
    char *b = (char*)malloc((unsigned)ed_len+1); if(!b) return;
    memcpy(b, ed_buf, (unsigned)ed_len); b[ed_len]=0;
    if(*n==UNDO_MAX){ free(st[0].buf); for(int i=1;i<UNDO_MAX;i++)st[i-1]=st[i]; (*n)--; }
    st[*n].buf=b; st[*n].len=ed_len; st[*n].caret=ed_caret; (*n)++;
}
static void ed_checkpoint(void){ snap_push(ud,&ud_n); snap_clear(rd,&rd_n); ed_dirty_flag=1; }  /* before a mutation */
static void snap_restore(ed_snap *from,int *fn, ed_snap *to,int *tn){
    if(*fn<=0) return;
    /* push current onto the other stack */
    char *cur=(char*)malloc((unsigned)ed_len+1);
    if(cur){ memcpy(cur,ed_buf,(unsigned)ed_len); cur[ed_len]=0; to[*tn].buf=cur; to[*tn].len=ed_len; to[*tn].caret=ed_caret; if(*tn<UNDO_MAX)(*tn)++; else free(cur); }
    (*fn)--; ed_snap *s=&from[*fn];
    memcpy(ed_buf,s->buf,(unsigned)s->len); ed_len=s->len; ed_buf[ed_len]=0; ed_caret=s->caret;
    free(s->buf); s->buf=0; ed_sel=-1; ed_dirty_flag=1;
}
static void ed_undo(void){ snap_restore(ud,&ud_n,rd,&rd_n); scpy(ed_status,"undo",sizeof ed_status); }
static void ed_redo(void){ snap_restore(rd,&rd_n,ud,&ud_n); scpy(ed_status,"redo",sizeof ed_status); }

static int ed_sel_lo(void){ int a=ed_sel,b=ed_caret; return a<b?a:b; }
static int ed_sel_hi(void){ int a=ed_sel,b=ed_caret; return a<b?b:a; }
static int ed_has_sel(void){ return ed_sel>=0 && ed_sel!=ed_caret; }
static void ed_del_range(int lo,int hi){ if(lo<0)lo=0; if(hi>ed_len)hi=ed_len; if(lo>=hi)return; int d=hi-lo; for(int i=lo;i+d<=ed_len;i++)ed_buf[i]=ed_buf[i+d]; ed_len-=d; ed_caret=lo; ed_buf[ed_len]=0; }
static void ed_copy(void){ if(!ed_has_sel())return; int lo=ed_sel_lo(),hi=ed_sel_hi(); sys_clip_set(ed_buf+lo,(unsigned)(hi-lo)); scpy(ed_status,"copied",sizeof ed_status); }
static void ed_cut(void){ if(!ed_has_sel())return; ed_copy(); ed_checkpoint(); ed_del_range(ed_sel_lo(),ed_sel_hi()); ed_sel=-1; scpy(ed_status,"cut",sizeof ed_status); }
static void ed_paste(void){
    static char cb[ED_MAX]; int n=sys_clip_get(cb,sizeof cb); if(n<=0)return; if(n>(int)sizeof cb)n=(int)sizeof cb;
    ed_checkpoint();
    if(ed_has_sel()) ed_del_range(ed_sel_lo(),ed_sel_hi());
    ed_sel=-1;
    for(int i=0;i<n && ed_len<ED_MAX-1;i++){ for(int j=ed_len;j>ed_caret;j--)ed_buf[j]=ed_buf[j-1]; ed_buf[ed_caret++]=cb[i]; ed_len++; }
    ed_buf[ed_len]=0; scpy(ed_status,"pasted",sizeof ed_status);
}

static void ed_new(void){ ed_buf[0]=0; ed_len=0; ed_caret=0; ed_top=0; ed_dirty_flag=0; scpy(ed_status,"new buffer",sizeof ed_status); }
static void ed_load(const char *path){
    scpy(ed_path,path,sizeof ed_path);
    int fd=sys_open(path,O_RDONLY);
    if(fd<0){ ed_new(); scpy(ed_status,"open failed",sizeof ed_status); return; }
    long n=sys_read(fd,ed_buf,ED_MAX-1); sys_close(fd);
    if(n<0)n=0; ed_buf[n]=0; ed_len=(int)n; ed_caret=0; ed_top=0; ed_dirty_flag=0;
    scpy(ed_status,"loaded ",sizeof ed_status); scat(ed_status,path);
}
static void ed_save(void){
    if(!ed_path[0]){ scpy(ed_status,"no path",sizeof ed_status); return; }
    int rc=sys_write_file(ed_path,ed_buf,(unsigned)ed_len);
    scpy(ed_status,rc<0?"save FAILED":"saved ",sizeof ed_status);
    if(rc>=0){ scat(ed_status,ed_path); ed_dirty_flag=0; }
}
static int ed_coalesce = -2;    /* caret after last insert -> group a typing run into one undo */
static void ed_insert(char ch){
    if(ed_len>=ED_MAX-1)return;
    if(ed_has_sel()){ ed_checkpoint(); ed_del_range(ed_sel_lo(),ed_sel_hi()); ed_sel=-1; ed_coalesce=-2; }
    else if(ed_caret!=ed_coalesce) ed_checkpoint();
    for(int i=ed_len;i>ed_caret;i--)ed_buf[i]=ed_buf[i-1];
    ed_buf[ed_caret++]=ch; ed_len++; ed_buf[ed_len]=0; ed_dirty_flag=1; ed_coalesce=ed_caret;
}
static void ed_backspace(void){
    if(ed_has_sel()){ ed_checkpoint(); ed_del_range(ed_sel_lo(),ed_sel_hi()); ed_sel=-1; ed_coalesce=-2; return; }
    if(ed_caret<=0)return;
    ed_checkpoint(); ed_coalesce=-2;
    for(int i=ed_caret-1;i<ed_len;i++)ed_buf[i]=ed_buf[i+1]; ed_caret--; ed_len--; ed_dirty_flag=1;
}
static void ed_rowcol(int idx,int *row,int *col){ int r=0,cc=0; for(int i=0;i<idx&&i<ed_len;i++){ if(ed_buf[i]=='\n'){r++;cc=0;}else cc++; } *row=r; *col=cc; }
static int ed_index_of(int row,int col){ int r=0,cc=0,i=0; for(;i<ed_len;i++){ if(r==row&&cc==col)return i; if(ed_buf[i]=='\n'){ if(r==row)return i; r++; cc=0; }else cc++; } return ed_len; }

static void ed_open_dialog(int mode){
    ed_dlg=mode;
    path_dir(ed_path[0]?ed_path:"/",ed_brz.cwd,sizeof ed_brz.cwd);
    ed_brz.sel=ed_brz.scroll=0; ed_brz.loaded=0; br_load(&ed_brz);
    if(mode==2){ if(ed_path[0])path_base(ed_path,ed_savename,sizeof ed_savename); else scpy(ed_savename,"untitled.txt",sizeof ed_savename); }
}

enum { A_NEW=1,A_OPEN,A_SAVE,A_SAVEAS,A_EXIT, A_CUT,A_COPY,A_PASTE,A_UNDO,A_REDO,A_SELALL, A_ABOUT };
static int ed_exit_req=0;
static void ed_do(int a){
    switch(a){
    case A_NEW:    ed_new(); ed_sel=-1; snap_clear(ud,&ud_n); snap_clear(rd,&rd_n); ed_coalesce=-2; break;
    case A_OPEN:   ed_open_dialog(1); break;
    case A_SAVE:   if(ed_path[0])ed_save(); else ed_open_dialog(2); break;
    case A_SAVEAS: ed_open_dialog(2); break;
    case A_EXIT:   ed_exit_req=1; break;
    case A_CUT:    ed_cut(); break;
    case A_COPY:   ed_copy(); break;
    case A_PASTE:  ed_paste(); break;
    case A_UNDO:   ed_undo(); break;
    case A_REDO:   ed_redo(); break;
    case A_SELALL: if(ed_len>0){ ed_sel=0; ed_caret=ed_len; } break;
    case A_ABOUT:  ed_about=1; break;
    }
}

static void ed_frame(gfx_surface *s,ui_ctx *u,int focused,int rpressed){
    int cw=s->w, ch=s->h;
    gfx_fill(s,0,0,cw,ch,RGB(0x16,0x1b,0x24));
    int tax=6,tay=UI_MENUBAR_H+4,taw=cw-12,tah=ch-tay-18;

    /* open/save dialog: modal, replaces the editor view */
    if(ed_dlg){
        char full[256];
        int r=br_dialog(&ed_brz,u,s,tax,tay,taw,tah,ed_dlg,ed_savename,BR_NAMW,full,sizeof full);
        if(r==1){ if(ed_dlg==1) ed_load(full); else { scpy(ed_path,full,sizeof ed_path); ed_save(); } ed_dlg=0; }
        else if(r==2){ ed_dlg=0; }
        gfx_str_clip(s,tax+4,ch-12,ed_status,UI_COL_MUTED,tax+taw);
        return;
    }

    /* while a menu/About/context menu is open, the editor ignores input (overlays
     * draw last) */
    int busy = (ed_menu>=0) || ed_about || ed_ctx;
    int kk = (busy||!focused) ? -1 : u->key;
    int mp = busy ? 0 : u->mpressed;
    int md = busy ? 0 : u->mdown;
    int rp = busy ? 0 : rpressed;

    gfx_fill(s,tax,tay,taw,tah,UI_COL_FIELD);
    gfx_outline(s,tax,tay,taw,tah,focused?UI_COL_BTN_ACT:RGB(0x07,0x09,0x0d));
    int vis_rows=(tah-4)/10, caret_row,caret_col; ed_rowcol(ed_caret,&caret_row,&caret_col);
    int sbw=12, txw=taw-sbw;     /* reserve the right strip for a scrollbar */
    int total_rows; { int rr,cc; ed_rowcol(ed_len,&rr,&cc); total_rows=rr+1; }
    int caretmoved=0;            /* only auto-scroll to the caret when it moved */

    if(mp && u->mx>=tax&&u->mx<tax+txw&&u->my>=tay&&u->my<tay+tah){
        int row=ed_top+(u->my-tay-2)/10, col=(u->mx-tax-4)/8; if(col<0)col=0;
        ed_caret=ed_index_of(row,col); ed_sel=ed_caret; u->got_input=1; caretmoved=1;
    } else if(md && ed_sel>=0 && u->mx>=tax&&u->mx<tax+txw&&u->my>=tay&&u->my<tay+tah){
        int row=ed_top+(u->my-tay-2)/10, col=(u->mx-tax-4)/8; if(col<0)col=0;
        ed_caret=ed_index_of(row,col); caretmoved=1;
    }
    /* right-click in the text area opens the edit context menu (at the click) */
    if(rp && u->mx>=tax&&u->mx<tax+txw&&u->my>=tay&&u->my<tay+tah){
        ed_ctx=1; ed_ctx_x=u->mx; ed_ctx_y=u->my;
    }

    if(kk>=0){
        int k=kk;
        if(k==3) ed_copy();
        else if(k==24) ed_cut();
        else if(k==22) ed_paste();
        else if(k==26) ed_undo();
        else if(k==25) ed_redo();
        else if(k==1){ if(ed_len>0){ ed_sel=0; ed_caret=ed_len; } }
        else if(k==19){ if(ed_path[0])ed_save(); else ed_open_dialog(2); }
        else if(k==15) ed_open_dialog(1);
        else if(k==14){ ed_new(); ed_sel=-1; snap_clear(ud,&ud_n); snap_clear(rd,&rd_n); }
        else if(k==8||k==127) ed_backspace();
        else if(k=='\r'||k=='\n') ed_insert('\n');
        else if(k==KEY_ARROW_LEFT){ ed_sel=-1; ed_coalesce=-2; if(ed_caret>0)ed_caret--; }
        else if(k==KEY_ARROW_RIGHT){ ed_sel=-1; ed_coalesce=-2; if(ed_caret<ed_len)ed_caret++; }
        else if(k==KEY_ARROW_UP){ ed_sel=-1; ed_coalesce=-2; if(caret_row>0)ed_caret=ed_index_of(caret_row-1,caret_col); }
        else if(k==KEY_ARROW_DOWN){ ed_sel=-1; ed_coalesce=-2; ed_caret=ed_index_of(caret_row+1,caret_col); }
        else if(k>=32&&k<127) ed_insert((char)k);
        ed_rowcol(ed_caret,&caret_row,&caret_col); u->got_input=1; caretmoved=1;
    }
    if(caretmoved){
        if(caret_row<ed_top)ed_top=caret_row;
        if(caret_row>=ed_top+vis_rows)ed_top=caret_row-vis_rows+1;
    }
    if(ed_top<0)ed_top=0;
    if(total_rows>0 && ed_top>total_rows-1)ed_top=total_rows-1;

    int slo=ed_has_sel()?ed_sel_lo():-1, shi=ed_has_sel()?ed_sel_hi():-1;
    int row=0,col=0;
    for(int i=0;i<=ed_len;i++){
        if(row>=ed_top+vis_rows)break;
        char ch2=ed_buf[i];
        if(row>=ed_top){
            int sx=tax+4+col*8, sy=tay+2+(row-ed_top)*10;
            if(i>=slo&&i<shi&&col*8<txw-8) gfx_fill(s,sx,sy,8,10,UI_COL_SEL);
            if(ch2&&ch2!='\n'&&col*8<txw-8) gfx_char(s,sx,sy,(unsigned char)ch2,COL_TEXT);
            if(i==ed_caret&&focused&&col*8<txw-8) gfx_fill(s,sx,sy,2,8,RGB(0xff,0xe0,0x60));
        }
        if(ch2=='\n'){ row++; col=0; } else col++;
    }
    /* vertical scrollbar down the reserved right strip (inert while a menu is up) */
    if(!busy) ui_vscroll(u,s,tax+txw,tay,sbw,tah,total_rows,vis_rows,&ed_top);
    else { ui_ctx z; for(unsigned i=0;i<sizeof z/sizeof(int);i++)((int*)&z)[i]=0; z.mx=z.my=-1;
           ui_vscroll(&z,s,tax+txw,tay,sbw,tah,total_rows,vis_rows,&ed_top); }
    gfx_str_clip(s,tax+4,ch-12,ed_status,UI_COL_MUTED,tax+taw);

    /* menu bar + About, drawn last so they overlay the editor */
    static const ui_menu_item fitems[]={{"New",A_NEW,1,"^N"},{"Open...",A_OPEN,1,"^O"},{"Save",A_SAVE,1,"^S"},{"Save As...",A_SAVEAS,1,0},{0,0,0,0},{"Exit",A_EXIT,1,0}};
    ui_menu_item eitems[]={{"Cut",A_CUT,ed_has_sel(),"^X"},{"Copy",A_COPY,ed_has_sel(),"^C"},{"Paste",A_PASTE,1,"^V"},{0,0,0,0},{"Undo",A_UNDO,ud_n>0,"^Z"},{"Redo",A_REDO,rd_n>0,"^Y"},{0,0,0,0},{"Select All",A_SELALL,ed_len>0,"^A"}};
    static const ui_menu_item hitems[]={{"About mxedit",A_ABOUT,1,0}};
    ui_menu menus[]={{"File",fitems,6},{"Edit",eitems,8},{"Help",hitems,1}};
    int act=ui_menubar(u,s,menus,3,&ed_menu);
    if(act) ed_do(act);

    static const char *al[]={"Makar text editor (mxedit)","(c) 2026 Arawn Davies  --  MIT","","Cut / Copy / Paste, Undo / Redo, selection.","Part of Makar OS."};
    ui_about(u,s,"About mxedit",al,5,&ed_about);

    /* right-click context menu (Cut/Copy/Paste/Undo/Redo), drawn last of all */
    ui_menu_item citems[]={{"Cut",A_CUT,ed_has_sel(),"^X"},{"Copy",A_COPY,ed_has_sel(),"^C"},{"Paste",A_PASTE,1,"^V"},{0,0,0,0},{"Undo",A_UNDO,ud_n>0,"^Z"},{"Redo",A_REDO,rd_n>0,"^Y"}};
    int cact=ui_context_menu(u,s,ed_ctx_x,ed_ctx_y,citems,6,&ed_ctx);
    if(cact) ed_do(cact);
}

int main(int argc,char**argv){
    mx_conn c;
    if(mx_connect(&c,argc,argv,620,420,MX_F_RESIZABLE)!=0) return 1;
    ed_new();
    for(int i=1;i+1<argc;i++) if(streq(argv[i],"-open")){ ed_load(argv[i+1]); break; }

    ui_ctx u; for(unsigned i=0;i<sizeof u/sizeof(int);i++)((int*)&u)[i]=0;
    int first=1,lmx=-1,lmy=-1,lfocus=-1;

    while(!c.closed && !ed_exit_req){
        mx_pump(&c);
        int key=mx_key(&c);
        int changed=first||key>=0||c.mpressed||c.mreleased||c.rpressed||c.mx!=lmx||c.my!=lmy||c.focused!=lfocus||c.resized;
        lmx=c.mx; lmy=c.my; lfocus=c.focused; first=0;
        if(!changed){ sys_yield(); continue; }
        ui_begin(&u,c.mx,c.my,c.mdown,c.mpressed,c.mreleased,key);
        ed_frame(&c.surf,&u,c.focused,c.rpressed);
        mx_present(&c);
        sys_yield();
    }
    mx_close(&c);
    return 0;
}
