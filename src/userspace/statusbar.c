/*
 * statusbar.c -- /apps/statusbar.elf
 *
 * Userspace renderer for the kernel's reserved bottom status row.  The kernel
 * provides only the mechanism: SYS_STATUSBAR reserves/frees the row and
 * SYS_PUTCH_AT paints status-row cells regardless of which VT is focused.
 * Everything about *what* the bar shows lives here (Linux/tmux model).
 *
 * Layout + widgets are configured by ~/.sbrc (re-read live so edits + the
 * post-login user change are picked up).  Each non-comment line is:
 *
 *     <section> <widget> [<widget> ...]        section = left | center | right
 *
 * Widgets: hostname user date time datetime uptime  (command/mem/rootfs are
 * reserved names for later phases -- they render empty for now).  Default
 * when ~/.sbrc is absent:  left hostname  /  right time.
 *
 * Alt+F5 toggles the whole bar (reserve <-> free the row).
 *
 * Freestanding (only syscall.h) so in-OS TCC can rebuild it.
 */

#include "syscall.h"

#define SB_CLR      ((VGA_BROWN << 4) | VGA_WHITE)   /* white on brown */
#define SB_MAXCELLS 256
#define SEC_MAX     128      /* rendered chars per section */
#define RC_MAX      512      /* ~/.sbrc bytes */

/* ---- tiny string helpers ------------------------------------------------ */
static unsigned int s_len(const char *s){ unsigned int n=0; while(s[n])n++; return n; }
static int s_eq(const char *a,const char *b){ while(*a&&*b){ if(*a!=*b)return 0; a++; b++; } return *a==*b; }
static void s_cat(char *d,unsigned int *o,unsigned int cap,const char *s)
{ while(*s && *o+1<cap) d[(*o)++]=*s++; d[*o]='\0'; }

/* ---- cell batching ------------------------------------------------------ */
static void put_cell(tty_cell_t *c,unsigned int *n,unsigned int col,unsigned int row,char ch,unsigned char clr)
{ if(*n>=SB_MAXCELLS)return; c[*n].col=(unsigned char)col; c[*n].row=(unsigned char)row;
  c[*n].ch=(unsigned char)ch; c[*n].clr=clr; (*n)++; }
static void put_str(tty_cell_t *c,unsigned int *n,unsigned int col,unsigned int row,const char *s,unsigned char clr)
{ while(*s) put_cell(c,n,col++,row,*s++,clr); }

/* ---- data sources ------------------------------------------------------- */

/* /proc/rtc -> "YYYY-MM-DD HH:MM:SS" (>=19 chars), into buf. */
static int read_rtc(char *buf,unsigned int cap)
{
    int fd=sys_open("/proc/rtc",O_RDONLY);
    if(fd<0)return -1;
    long r=sys_read(fd,buf,cap-1); sys_close(fd);
    if(r<0)return -1;
    buf[r]='\0';
    return (int)r;
}

static char g_host[48];
static char g_user[48];

/* Append the named widget's current value to out[]. */
static void widget(const char *name,char *out,unsigned int *o,unsigned int cap)
{
    if(s_eq(name,"hostname")){ s_cat(out,o,cap,g_host); return; }
    if(s_eq(name,"user")){ s_cat(out,o,cap,g_user); return; }

    if(s_eq(name,"date")||s_eq(name,"time")||s_eq(name,"datetime")){
        char rtc[40];
        if(read_rtc(rtc,sizeof(rtc))<19) return;
        char tmp[24]; unsigned int t=0;
        if(s_eq(name,"date")){
            /* YYYY-MM-DD */
            for(int i=0;i<10;i++) tmp[t++]=rtc[i];
        } else if(s_eq(name,"time")){
            tmp[t++]=rtc[11];tmp[t++]=rtc[12];tmp[t++]=':';
            tmp[t++]=rtc[14];tmp[t++]=rtc[15];tmp[t++]=':';
            tmp[t++]=rtc[17];tmp[t++]=rtc[18];
        } else { /* datetime */
            for(int i=0;i<19;i++) tmp[t++]=rtc[i];
        }
        tmp[t]='\0';
        s_cat(out,o,cap,tmp);
        return;
    }

    if(s_eq(name,"uptime")){
        unsigned int secs=sys_uptime()/100u;
        unsigned int h=secs/3600u, m=(secs/60u)%60u, sc=secs%60u;
        char tmp[16]; unsigned int t=0;
        const char *up="up "; while(*up) tmp[t++]=*up++;
        tmp[t++]=(char)('0'+h/10);tmp[t++]=(char)('0'+h%10);tmp[t++]=':';
        tmp[t++]=(char)('0'+m/10);tmp[t++]=(char)('0'+m%10);tmp[t++]=':';
        tmp[t++]=(char)('0'+sc/10);tmp[t++]=(char)('0'+sc%10);tmp[t]='\0';
        s_cat(out,o,cap,tmp);
        return;
    }

    /* command / mem / rootfs: reserved for a later phase -> render empty. */
}

/* ---- ~/.sbrc -> three section widget-lists ------------------------------ */
static char g_left[SEC_MAX];      /* space-separated widget names */
static char g_center[SEC_MAX];
static char g_right[SEC_MAX];

static void set_default_layout(void)
{
    g_left[0]='h';g_left[1]='o';g_left[2]='s';g_left[3]='t';g_left[4]='n';
    g_left[5]='a';g_left[6]='m';g_left[7]='e';g_left[8]='\0';
    g_center[0]='\0';
    g_right[0]='t';g_right[1]='i';g_right[2]='m';g_right[3]='e';g_right[4]='\0';
}

/* Build "/home/<user>/.sbrc" (root -> /root/.sbrc) into path. */
static void sbrc_path(char *path,unsigned int cap)
{
    unsigned int o=0;
    if(s_eq(g_user,"root")){ s_cat(path,&o,cap,"/root"); }
    else { s_cat(path,&o,cap,"/home/"); s_cat(path,&o,cap,g_user); }
    s_cat(path,&o,cap,"/.sbrc");
}

static void load_sbrc(void)
{
    set_default_layout();

    char path[80]; sbrc_path(path,sizeof(path));
    int fd=sys_open(path,O_RDONLY);
    if(fd<0) return;                 /* no config -> keep defaults */
    char buf[RC_MAX]; long r=sys_read(fd,buf,sizeof(buf)-1); sys_close(fd);
    if(r<=0) return;
    buf[r]='\0';

    int saw_any=0;
    long i=0;
    while(i<r){
        char line[SEC_MAX+16]; int li=0;
        while(i<r && buf[i]!='\n'){ if(li<(int)sizeof(line)-1) line[li++]=buf[i]; i++; }
        line[li]='\0'; if(i<r) i++;

        const char *p=line; while(*p==' '||*p=='\t') p++;
        if(*p=='\0'||*p=='#') continue;

        /* first token = section */
        char sec[12]; int s=0;
        while(*p && *p!=' ' && *p!='\t' && s<(int)sizeof(sec)-1) sec[s++]=*p++;
        sec[s]='\0';
        char *dst=0;
        if(s_eq(sec,"left")) dst=g_left;
        else if(s_eq(sec,"center")) dst=g_center;
        else if(s_eq(sec,"right")) dst=g_right;
        else continue;

        /* rest of line (the widget list) trimmed */
        while(*p==' '||*p=='\t') p++;
        if(!saw_any){ g_left[0]=g_center[0]=g_right[0]='\0'; saw_any=1; }
        unsigned int o=0; dst[0]='\0';
        s_cat(dst,&o,SEC_MAX,p);
    }
}

/* Render a section's widget list into out[] (widgets joined by "  "). */
static void build_section(const char *list,char *out)
{
    unsigned int o=0; out[0]='\0';
    const char *p=list;
    while(*p){
        while(*p==' '||*p=='\t') p++;
        if(!*p) break;
        char name[24]; int n=0;
        while(*p && *p!=' ' && *p!='\t' && n<(int)sizeof(name)-1) name[n++]=*p++;
        name[n]='\0';
        unsigned int before=o;
        if(o) s_cat(out,&o,SEC_MAX,"  ");
        unsigned int after_sep=o;
        widget(name,out,&o,SEC_MAX);
        if(o==after_sep) o=before, out[o]='\0';   /* widget empty -> drop sep */
    }
}

static void draw(void)
{
    unsigned int size=(unsigned int)sys_term_size();
    unsigned int cols=(size>>16)&0xFFFFu;
    unsigned int row =size&0xFFFFu;        /* status row = drawable rows */
    if(cols<12)return;
    if(cols>SB_MAXCELLS) cols=SB_MAXCELLS;

    char left[SEC_MAX],center[SEC_MAX],right[SEC_MAX];
    build_section(g_left,left);
    build_section(g_center,center);
    build_section(g_right,right);

    tty_cell_t cells[SB_MAXCELLS]; unsigned int n=0;
    for(unsigned int c=0;c<cols;c++) put_cell(cells,&n,c,row,' ',SB_CLR);

    if(left[0])   put_str(cells,&n,1,row,left,SB_CLR);

    unsigned int rl=s_len(right);
    if(rl && cols>rl+1) put_str(cells,&n,cols-rl-1,row,right,SB_CLR);

    unsigned int cl=s_len(center);
    if(cl && cols>cl){ unsigned int cc=(cols-cl)/2u; put_str(cells,&n,cc,row,center,SB_CLR); }

    sys_putch_at(cells,n);
}

int main(void)
{
    if(sys_gethostname(g_host,sizeof(g_host))<=0){
        g_host[0]='m';g_host[1]='a';g_host[2]='k';g_host[3]='a';g_host[4]='r';g_host[5]='\0';
    }
    g_user[0]='\0';
    set_default_layout();

    unsigned int last=sys_uptime();
    unsigned int rc_next=0;             /* force an immediate ~/.sbrc load */
    for(;;){
        if(sys_vt_clock_request()>0)
            sys_statusbar_set(!sys_statusbar_enabled());

        /* Re-read user + ~/.sbrc every ~3 s (picks up login + edits). */
        unsigned int now=sys_uptime();
        if(now>=rc_next){
            if(sys_whoami(g_user,sizeof(g_user))<=0){ g_user[0]='u';g_user[1]='s';g_user[2]='e';g_user[3]='r';g_user[4]='\0'; }
            load_sbrc();
            rc_next=now+300u;           /* 3 s at 100 Hz */
        }

        if(sys_statusbar_enabled())
            draw();

        unsigned int next=last+20u;     /* ~0.2 s frame */
        while(sys_uptime()<next) sys_yield();
        last=sys_uptime();
    }
}
