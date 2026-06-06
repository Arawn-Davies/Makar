/*
 * mxfiles.elf -- file browser, as a makx client.  Navigates the filesystem with
 * the shared gui_browser model and draws with the gui_ui widgets, into its makx
 * surface.  Was the W_FILES window kind in the old monolithic wm.c.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "gui_ui.h"
#include "gui_browser.h"
#include "makx.h"

static void scpy(char *d,const char *s,int max){int i=0;while(s[i]&&i<max-1){d[i]=s[i];i++;}d[i]=0;}

static browser g_files;
static char    pathbox[256];

static void open_sel(void){
    if(g_files.sel<0||g_files.sel>=g_files.n) return;
    if(br_sel_isdir(&g_files)) br_enter_sel(&g_files);
    /* a regular file: navigation-only here -- the Editor opens files via its
     * own Open dialog (each client is self-contained). */
}

int main(int argc,char**argv){
    mx_conn c;
    if(mx_connect(&c,argc,argv,560,380,MX_F_RESIZABLE)!=0) return 1;
    scpy(g_files.cwd,"/",sizeof g_files.cwd); br_load(&g_files);

    ui_ctx u; for(unsigned i=0;i<sizeof u/sizeof(int);i++)((int*)&u)[i]=0;
    int first=1, lmx=-1, lmy=-1, lfocus=-1;

    while(!c.closed){
        mx_pump(&c);
        int key=mx_key(&c);
        int changed = first || key>=0 || c.mpressed || c.mreleased ||
                      c.mx!=lmx || c.my!=lmy || c.focused!=lfocus||c.resized;
        lmx=c.mx; lmy=c.my; lfocus=c.focused; first=0;
        if(!changed){ sys_yield(); continue; }

        gfx_surface *s=&c.surf;
        gfx_fill(s,0,0,s->w,s->h,UI_COL_FIELD);
        ui_begin(&u,c.mx,c.my,c.mdown,c.mpressed,c.mreleased,
                 (key=='\t')?-1:key);

        int up_c=ui_button(&u,s,6,6,52,20,"Up");
        int op_c=ui_button(&u,s,66,6,64,20,"Open");
        int rf_c=ui_button(&u,s,138,6,72,20,"Refresh");
        int pbx=218, pbw=s->w-12-pbx-52; if(pbw<60)pbw=60;
        ui_textbox(&u,s,pbx,6,pbw,20,pathbox,(int)sizeof pathbox);
        int go_c=ui_button(&u,s,pbx+pbw+4,6,48,20,"Go");

        if(up_c) br_up(&g_files);
        if(rf_c) br_load(&g_files);
        if(go_c&&pathbox[0]){ scpy(g_files.cwd,pathbox,sizeof g_files.cwd); g_files.sel=g_files.scroll=0; br_load(&g_files); }

        ui_label(&u,s,6,30,g_files.cwd,UI_COL_MUTED);

        int lx=6, ly=50, lw=s->w-12, lh=s->h-56, prev=g_files.sel;
        ui_listbox(&u,s,lx,ly,lw,lh,g_files.ptr,g_files.n,&g_files.sel,&g_files.scroll);
        if(op_c) open_sel();
        else if(c.focused && key=='\n') open_sel();
        else if(c.focused && c.mpressed && prev==g_files.sel &&
                c.mx>=lx&&c.mx<lx+lw&&c.my>=ly&&c.my<ly+lh) open_sel();

        mx_present(&c);
        sys_yield();
    }
    mx_close(&c);
    return 0;
}
