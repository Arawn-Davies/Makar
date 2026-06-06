/*
 * mxedit.elf -- text editor, as a makx client.  A multi-line buffer with a
 * toolbar (Open/Save/Save As/New) and an open/save dialog built on the shared
 * gui_browser model, drawn with gui_ui into its makx surface.  Was the W_EDITOR
 * window kind in the old monolithic wm.c.  `-open <path>` preloads a file.
 */
#include "syscall.h"
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
static void ed_insert(char ch){ if(ed_len>=ED_MAX-1)return; for(int i=ed_len;i>ed_caret;i--)ed_buf[i]=ed_buf[i-1]; ed_buf[ed_caret++]=ch; ed_len++; ed_buf[ed_len]=0; ed_dirty_flag=1; }
static void ed_backspace(void){ if(ed_caret<=0)return; for(int i=ed_caret-1;i<ed_len;i++)ed_buf[i]=ed_buf[i+1]; ed_caret--; ed_len--; ed_dirty_flag=1; }
static void ed_rowcol(int idx,int *row,int *col){ int r=0,cc=0; for(int i=0;i<idx&&i<ed_len;i++){ if(ed_buf[i]=='\n'){r++;cc=0;}else cc++; } *row=r; *col=cc; }
static int ed_index_of(int row,int col){ int r=0,cc=0,i=0; for(;i<ed_len;i++){ if(r==row&&cc==col)return i; if(ed_buf[i]=='\n'){ if(r==row)return i; r++; cc=0; }else cc++; } return ed_len; }

static void ed_open_dialog(int mode){
    ed_dlg=mode;
    path_dir(ed_path[0]?ed_path:"/",ed_brz.cwd,sizeof ed_brz.cwd);
    ed_brz.sel=ed_brz.scroll=0; ed_brz.loaded=0; br_load(&ed_brz);
    if(mode==2){ if(ed_path[0])path_base(ed_path,ed_savename,sizeof ed_savename); else scpy(ed_savename,"untitled.txt",sizeof ed_savename); }
}

static void ed_frame(gfx_surface *s,ui_ctx *u,int focused){
    int cw=s->w, ch=s->h;
    gfx_fill(s,0,0,cw,ch,RGB(0x16,0x1b,0x24));
    int bx=6,by=6;
    int open_c=ui_button(u,s,bx,by,60,20,"Open");
    int save_c=ui_button(u,s,bx+66,by,60,20,"Save");
    int saveas_c=ui_button(u,s,bx+132,by,76,20,"Save As");
    int new_c=ui_button(u,s,bx+216,by,52,20,"New");
    ui_label(u,s,bx+276,by+6,ed_path[0]?ed_path:"(unsaved)",UI_COL_MUTED);
    if(new_c){ ed_new(); ed_dlg=0; }
    if(open_c)ed_open_dialog(1);
    if(saveas_c)ed_open_dialog(2);
    if(save_c){ if(ed_path[0])ed_save(); else ed_open_dialog(2); }

    int tax=6,tay=34,taw=cw-12,tah=ch-34-18;
    if(ed_dlg){
        char full[256];
        int r=br_dialog(&ed_brz,u,s,tax,tay,taw,tah,ed_dlg,ed_savename,BR_NAMW,full,sizeof full);
        if(r==1){
            if(ed_dlg==1) ed_load(full);
            else { scpy(ed_path,full,sizeof ed_path); ed_save(); }
            ed_dlg=0;
        } else if(r==2){ ed_dlg=0; }
        gfx_str_clip(s,tax+4,tay+tah+6,ed_status,UI_COL_MUTED,tax+taw);
        return;
    }
    gfx_fill(s,tax,tay,taw,tah,UI_COL_FIELD);
    gfx_outline(s,tax,tay,taw,tah,focused?UI_COL_BTN_ACT:RGB(0x07,0x09,0x0d));

    int vis_rows=(tah-4)/10, caret_row,caret_col; ed_rowcol(ed_caret,&caret_row,&caret_col);
    if(focused&&u->mpressed&&u->mx>=tax&&u->mx<tax+taw&&u->my>=tay&&u->my<tay+tah){
        int row=ed_top+(u->my-tay-2)/10, col=(u->mx-tax-4)/8; if(col<0)col=0;
        ed_caret=ed_index_of(row,col); ed_rowcol(ed_caret,&caret_row,&caret_col); u->got_input=1;
    }
    if(focused&&u->key>=0){
        int k=u->key;
        if(k==8||k==127)ed_backspace();
        else if(k=='\r'||k=='\n')ed_insert('\n');
        else if(k==KEY_ARROW_LEFT){ if(ed_caret>0)ed_caret--; }
        else if(k==KEY_ARROW_RIGHT){ if(ed_caret<ed_len)ed_caret++; }
        else if(k==KEY_ARROW_UP){ if(caret_row>0)ed_caret=ed_index_of(caret_row-1,caret_col); }
        else if(k==KEY_ARROW_DOWN)ed_caret=ed_index_of(caret_row+1,caret_col);
        else if(k>=32&&k<127)ed_insert((char)k);
        ed_rowcol(ed_caret,&caret_row,&caret_col); u->got_input=1;
    }
    if(caret_row<ed_top)ed_top=caret_row;
    if(caret_row>=ed_top+vis_rows)ed_top=caret_row-vis_rows+1;
    if(ed_top<0)ed_top=0;

    int row=0,col=0,scr_row=0;
    for(int i=0;i<=ed_len&&scr_row<vis_rows;i++){
        if(row>=ed_top&&row<ed_top+vis_rows)scr_row=row-ed_top;
        char ch2=ed_buf[i];
        if(row>=ed_top&&(ch2&&ch2!='\n')&&col*8<taw-8)gfx_char(s,tax+4+col*8,tay+2+(row-ed_top)*10,(unsigned char)ch2,COL_TEXT);
        if(i==ed_caret&&focused&&row>=ed_top&&row<ed_top+vis_rows)gfx_fill(s,tax+4+col*8,tay+2+(row-ed_top)*10,2,8,RGB(0xff,0xe0,0x60));
        if(ch2=='\n'){ row++; col=0; }else col++;
        if(row>=ed_top+vis_rows)break;
    }
    gfx_str_clip(s,tax+4,tay+tah+6,ed_status,UI_COL_MUTED,tax+taw);
}

int main(int argc,char**argv){
    mx_conn c;
    if(mx_connect(&c,argc,argv,620,420)!=0) return 1;
    ed_new();
    for(int i=1;i+1<argc;i++) if(streq(argv[i],"-open")){ ed_load(argv[i+1]); break; }

    ui_ctx u; for(unsigned i=0;i<sizeof u/sizeof(int);i++)((int*)&u)[i]=0;
    int first=1,lmx=-1,lmy=-1,lfocus=-1;

    while(!c.closed){
        mx_pump(&c);
        int key=mx_key(&c);
        int changed=first||key>=0||c.mpressed||c.mreleased||c.mx!=lmx||c.my!=lmy||c.focused!=lfocus;
        lmx=c.mx; lmy=c.my; lfocus=c.focused; first=0;
        if(!changed){ sys_yield(); continue; }
        ui_begin(&u,c.mx,c.my,c.mdown,c.mpressed,c.mreleased,key);
        ed_frame(&c.surf,&u,c.focused);
        mx_present(&c);
        sys_yield();
    }
    mx_close(&c);
    return 0;
}
