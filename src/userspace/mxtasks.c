/*
 * mxtasks.elf -- task manager, as a makx client.  Lists /proc/tasks, kills the
 * selected pid, and a slider sets the auto-refresh interval.  Was the W_TASKS
 * window kind in the old monolithic wm.c.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "gui_ui.h"
#include "makx.h"

static char *u2s(unsigned int v,char *out){char t[12];int i=0;if(!v)t[i++]='0';while(v){t[i++]=(char)('0'+v%10u);v/=10u;}int j=0;while(i)out[j++]=t[--i];out[j]=0;return out+j;}

#define TK_MAX 64
static char tk_row[TK_MAX][72];
static const char *tk_ptr[TK_MAX];
static int  tk_pid[TK_MAX];
static int  tk_n, tk_sel, tk_scroll, tk_interval=1;
static unsigned tk_next;
static char tk_buf[4096];

static void tasks_load(void){
    int fd=sys_open("/proc/tasks",O_RDONLY); tk_n=0;
    if(fd<0)return;
    long n=sys_read(fd,tk_buf,sizeof tk_buf-1); sys_close(fd);
    if(n<0)n=0; tk_buf[n]=0;
    char *p=tk_buf; int line=0;
    while(*p&&tk_n<TK_MAX){
        char *e=p; while(*e&&*e!='\n')e++;
        int len=(int)(e-p);
        if(line>0&&len>0){
            int copy=len<71?len:71;
            for(int i=0;i<copy;i++)tk_row[tk_n][i]=p[i];
            tk_row[tk_n][copy]=0;
            int v=0;char *q=p;while(*q==' ')q++;while(*q>='0'&&*q<='9'){v=v*10+(*q-'0');q++;}
            tk_pid[tk_n]=v; tk_ptr[tk_n]=tk_row[tk_n]; tk_n++;
        }
        if(!*e)break; p=e+1; line++;
    }
    if(tk_sel>=tk_n)tk_sel=tk_n?tk_n-1:0;
}

int main(int argc,char**argv){
    mx_conn c;
    if(mx_connect(&c,argc,argv,560,360,MX_F_RESIZABLE)!=0) return 1;
    tasks_load();
    ui_ctx u; for(unsigned i=0;i<sizeof u/sizeof(int);i++)((int*)&u)[i]=0;
    int first=1, lmx=-1, lmy=-1, lfocus=-1;

    while(!c.closed){
        mx_pump(&c);
        int key=mx_key(&c);
        unsigned now=sys_uptime();
        int tick = (tk_n==0 || (int)(now-tk_next)>=0);
        if(tick){ tasks_load(); tk_next=now+(unsigned)(tk_interval<1?1:tk_interval)*100u; }
        int changed = first||tick||key>=0||c.mpressed||c.mreleased||c.mx!=lmx||c.my!=lmy||c.focused!=lfocus||c.resized;
        lmx=c.mx; lmy=c.my; lfocus=c.focused; first=0;
        if(!changed){ sys_yield(); continue; }

        gfx_surface *s=&c.surf;
        gfx_fill(s,0,0,s->w,s->h,UI_COL_FIELD);
        ui_begin(&u,c.mx,c.my,c.mdown,c.mpressed,c.mreleased,key);

        int kill_c=ui_button(&u,s,6,6,72,20,"Kill");
        int rf_c  =ui_button(&u,s,86,6,72,20,"Refresh");
        ui_label(&u,s,170,12,"refresh(s)",UI_COL_MUTED);
        ui_slider(&u,s,258,6,120,20,&tk_interval,1,10);
        { char t[8]; u2s((unsigned)tk_interval,t); ui_label(&u,s,386,12,t,UI_COL_TEXT); }

        if(rf_c) tasks_load();
        if(kill_c&&tk_sel>=0&&tk_sel<tk_n&&tk_pid[tk_sel]>0){ sys_kill(tk_pid[tk_sel],SIGKILL); tasks_load(); }

        ui_listbox(&u,s,6,34,s->w-12,s->h-40,tk_ptr,tk_n,&tk_sel,&tk_scroll);

        mx_present(&c);
        sys_yield();
    }
    mx_close(&c);
    return 0;
}
