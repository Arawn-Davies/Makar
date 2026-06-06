/*
 * statusbar.c -- /apps/statusbar.elf
 *
 * Userspace renderer for the kernel's reserved bottom status row.  The kernel
 * provides only the mechanism: SYS_STATUSBAR reserves/frees the row and
 * SYS_PUTCH_AT paints status-row cells regardless of which VT is focused.
 * Everything about *what* the bar shows lives here (Linux/tmux model).
 *
 * Layout + widgets are configured by ~/.sbrc (re-read live so edits + the
 * post-login user change are picked up).  The bar is PER-TAB: each VT can own
 * its layout via ~/.sbrc.tty<N> (the /dev/ttyN number; root console = tty0),
 * which overrides the shared ~/.sbrc for that tab.  The active tab is read
 * from sys_vt_state, so switching VTs re-renders that tab's own bar -- no new
 * syscall surface.  Each non-comment line is:
 *
 *     <section> <widget> [<widget> ...]        section = left | center | right
 *
 * Widgets: hostname user date time datetime uptime cpu mem rootfs command tabs.
 * (`cpu`/`mem` = busy/used % -- same source + calc as maktop; `rootfs` =
 *  used/total MiB (ext2-only, empty on FAT32/ISO9660); `command` = the active
 *  VT's foreground task; `tabs` = the makmux VT strip.)
 * Default ~/.sbrc:  left command / center tabs / right cpu mem rootfs time.
 *
 * Alt+F5/F6 switching is kernel-owned: Alt+F5 surfaces mak.sh0 + this
 * statusbar, Alt+F6 hides the text console/statusbar and restores the GUI.
 *
 * Freestanding (only syscall.h) so in-OS TCC can rebuild it.
 */

#include "syscall.h"

#define SB_CLR      ((VGA_BROWN << 4) | VGA_WHITE)    /* white on brown */
#define SB_TAB_ACT  ((VGA_YELLOW << 4) | VGA_BLACK)   /* active VT tab  */
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

static void sb_uitoa(unsigned int v,char *buf)
{
    char t[12]; int i=0;
    if(v==0){ buf[0]='0'; buf[1]='\0'; return; }
    while(v){ t[i++]=(char)('0'+v%10); v/=10; }
    int j=0; while(i>0) buf[j++]=t[--i]; buf[j]='\0';
}

/* Value (kB) of a "Label:   N kB" line in /proc/meminfo, or 0. */
static unsigned int meminfo_kb(const char *label)
{
    char buf[512]; int fd=sys_open("/proc/meminfo",O_RDONLY);
    if(fd<0) return 0;
    long r=sys_read(fd,buf,sizeof(buf)-1); sys_close(fd);
    if(r<=0) return 0; buf[r]='\0';
    unsigned int ll=0; while(label[ll]) ll++;
    for(long i=0;i<r;){
        long j=0; while(j<(long)ll && buf[i+j]==label[j]) j++;
        if(j==(long)ll && buf[i+ll]==':'){
            const char *p=buf+i+ll+1; while(*p==' ') p++;
            unsigned int v=0; while(*p>='0'&&*p<='9'){ v=v*10+(unsigned int)(*p-'0'); p++; }
            return v;
        }
        while(i<r && buf[i]!='\n') i++;
        i++;
    }
    return 0;
}

/* Foreground command of the active VT: the live task with that tty and the
 * highest pid (a running child outranks its shell).  Empty if none. */
static void active_command(char *out,unsigned int cap)
{
    out[0]='\0';
    unsigned int active=((unsigned int)sys_vt_state()>>16)&0xFFFFu;
    char buf[1024]; int fd=sys_open("/proc/tasks",O_RDONLY);
    if(fd<0) return;
    long r=sys_read(fd,buf,sizeof(buf)-1); sys_close(fd);
    if(r<=0) return; buf[r]='\0';

    long i=0; int line=0; int best_pid=-1; char best[20]; best[0]='\0';
    int root_console = active == 9u;   /* VTTY_ROOT_SLOT (tty0) */
    while(i<r){
        char tok[5][20]; int nt=0;
        while(i<r && buf[i]!='\n'){
            while(i<r && (buf[i]==' '||buf[i]=='\t')) i++;
            if(i>=r||buf[i]=='\n') break;
            int tl=0;
            while(i<r&&buf[i]!=' '&&buf[i]!='\t'&&buf[i]!='\n'){
                if(nt<5 && tl<19) tok[nt][tl++]=buf[i];
                i++;
            }
            if(nt<5){ tok[nt][tl]='\0'; nt++; }
        }
        if(i<r) i++;
        if(line++==0) continue;                 /* header row */
        if(nt<4 || tok[3][0]=='-') continue;     /* no tty */
        unsigned int tn=0; for(int k=0;tok[3][k];k++) tn=tn*10+(unsigned int)(tok[3][k]-'0');
        if(tn!=active) continue;
        int pid=0; for(int k=0;tok[0][k];k++) pid=pid*10+(tok[0][k]-'0');
        if((!root_console && pid>best_pid) || (root_console && (best_pid<0 || pid<best_pid))){
            best_pid=pid; int b=0; while(tok[1][b]&&b<19){best[b]=tok[1][b];b++;} best[b]='\0';
        }
    }
    if(best[0]){ unsigned int o=0; s_cat(out,&o,cap,best); }
}

/* Sum of TICKS across all tasks except the idle task (pid 1), from
 * /proc/tasks -- maktop's CPU% basis. */
static unsigned int proc_busy_ticks(void)
{
    char buf[1024]; int fd=sys_open("/proc/tasks",O_RDONLY);
    if(fd<0) return 0;
    long r=sys_read(fd,buf,sizeof(buf)-1); sys_close(fd);
    if(r<=0) return 0; buf[r]='\0';
    long i=0; int line=0; unsigned int sum=0;
    while(i<r){
        char tok[6][20]; int nt=0;
        while(i<r && buf[i]!='\n'){
            while(i<r&&(buf[i]==' '||buf[i]=='\t')) i++;
            if(i>=r||buf[i]=='\n') break;
            int tl=0;
            while(i<r&&buf[i]!=' '&&buf[i]!='\t'&&buf[i]!='\n'){ if(nt<6&&tl<19) tok[nt][tl++]=buf[i]; i++; }
            if(nt<6){ tok[nt][tl]='\0'; nt++; }
        }
        if(i<r) i++;
        if(line++==0) continue;
        if(nt<5) continue;
        if(tok[0][0]=='1' && tok[0][1]=='\0') continue;     /* skip idle (pid 1) */
        unsigned int v=0; for(int k=0;tok[4][k];k++) v=v*10+(unsigned int)(tok[4][k]-'0');
        sum+=v;
    }
    return sum;
}

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

    if(s_eq(name,"mem")){
        /* maktop's calc: (MemTotal - MemFree) / MemTotal as a percentage. */
        unsigned int total=meminfo_kb("MemTotal"), freekb=meminfo_kb("MemFree");
        if(total){
            unsigned int pct=(total>freekb)?(total-freekb)*100u/total:0u;
            char b[8]; sb_uitoa(pct,b);
            s_cat(out,o,cap,"MEM "); s_cat(out,o,cap,b); s_cat(out,o,cap,"%");
        }
        return;
    }

    if(s_eq(name,"command")){
        char cmd[20]; active_command(cmd,sizeof(cmd));
        if(cmd[0]) s_cat(out,o,cap,cmd);
        return;
    }

    if(s_eq(name,"cpu")){
        /* maktop's calc: non-idle task ticks delta over real-time (uptime)
         * delta, as a percentage. */
        static unsigned int last_busy=0,last_up=0; static int have=0;
        unsigned int busy=proc_busy_ticks(), up=sys_uptime(), pct=0;
        if(have && up>last_up){
            unsigned int dt=up-last_up;
            unsigned int db=(busy>last_busy)?(busy-last_busy):0u;
            pct=db*100u/dt; if(pct>100u) pct=100u;
        }
        last_busy=busy; last_up=up; have=1;
        char b[8]; sb_uitoa(pct,b);
        s_cat(out,o,cap,"CPU "); s_cat(out,o,cap,b); s_cat(out,o,cap,"%");
        return;
    }

    if(s_eq(name,"rootfs")){
        unsigned int t=0,fr=0;
        if(sys_statfs(&t,&fr)==0 && t){
            char a[12],b[12]; sb_uitoa((t-fr)/1024u,a); sb_uitoa(t/1024u,b);
            s_cat(out,o,cap,"ROOT "); s_cat(out,o,cap,a);
            s_cat(out,o,cap,"/"); s_cat(out,o,cap,b); s_cat(out,o,cap,"M");
        }
        return;
    }
}

/* ---- ~/.sbrc -> three section widget-lists ------------------------------ */
static char g_left[SEC_MAX];      /* space-separated widget names */
static char g_center[SEC_MAX];
static char g_right[SEC_MAX];

static void set_default_layout(void)
{
    unsigned int o;
    o=0; s_cat(g_left,&o,SEC_MAX,"command");    /* active VT's foreground exe */
    o=0; s_cat(g_center,&o,SEC_MAX,"tabs");     /* makmux VT tabs */
    o=0; s_cat(g_right,&o,SEC_MAX,"cpu mem rootfs time");
}

/* Build "/home/<user>/.sbrc" (root -> /root/.sbrc) into path, optionally with
 * a per-tab suffix ".ttyN" so each VT can carry its OWN status-bar layout.
 * suffix_tty < 0 selects the shared ~/.sbrc; >= 0 selects ~/.sbrc.tty<N>. */
static void sbrc_path(char *path,unsigned int cap,int suffix_tty)
{
    unsigned int o=0;
    if(s_eq(g_user,"root")){ s_cat(path,&o,cap,"/root"); }
    else { s_cat(path,&o,cap,"/home/"); s_cat(path,&o,cap,g_user); }
    s_cat(path,&o,cap,"/.sbrc");
    if(suffix_tty>=0){
        char num[12]; sb_uitoa((unsigned int)suffix_tty,num);
        s_cat(path,&o,cap,".tty");
        s_cat(path,&o,cap,num);
    }
}

/* Parse one ~/.sbrc-format file at `path` into the section lists.  Returns 1
 * if the file existed and was read, 0 otherwise (caller keeps defaults). */
static int load_sbrc_file(const char *path)
{
    int fd=sys_open(path,O_RDONLY);
    if(fd<0) return 0;               /* no config -> keep defaults */
    char buf[RC_MAX]; long r=sys_read(fd,buf,sizeof(buf)-1); sys_close(fd);
    if(r<=0) return 0;
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
    return 1;
}

/* Load the layout for the currently-active tab.  Each VT can own its bar:
 * ~/.sbrc.tty<N> overrides the shared ~/.sbrc for tab N; if neither exists the
 * built-in default layout is used.  tty<N> is 1-based (the /dev/ttyN number);
 * the root console (tab 0) uses ~/.sbrc.tty0.  Picking the file off the active
 * tab is what makes the bar per-tab without any new syscall surface. */
static void load_sbrc(int active_tty)
{
    set_default_layout();

    char path[96];
    sbrc_path(path,sizeof(path),active_tty);   /* per-tab override first */
    if(load_sbrc_file(path)) return;
    sbrc_path(path,sizeof(path),-1);           /* fall back to shared ~/.sbrc */
    load_sbrc_file(path);
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

/* True if a section's widget list contains the token `tok`. */
static int sb_has(const char *list,const char *tok)
{
    const char *p=list;
    while(*p){
        while(*p==' '||*p=='\t') p++;
        if(!*p) break;
        const char *q=tok; const char *s=p;
        while(*s && *s!=' ' && *s!='\t' && *q && *s==*q){ s++; q++; }
        if(*q=='\0' && (*s=='\0'||*s==' '||*s=='\t')) return 1;
        while(*p && *p!=' ' && *p!='\t') p++;
    }
    return 0;
}

/* Render the live VT tabs centred within the [lo,hi) column window (the gap
 * between the left and right sections, so the tabs never paint over the
 * resource widgets).  Active highlighted; shells show "VTn", named app-tabs
 * show their name.  Tabs that don't fit the window are clipped.  No-op if
 * makmux isn't running (mask == 0).  Returns 1 if it drew tabs. */
static int render_tabs(tty_cell_t *cells,unsigned int *n,unsigned int row,
                       unsigned int lo,unsigned int hi)
{
    unsigned int state=(unsigned int)sys_vt_state();
    unsigned int active=(state>>16)&0xFFFFu;
    unsigned int mask=state&0xFFFFu;
    if(!mask || hi<=lo) return 0;

    char  lab[9][18];
    int   idx[9], cnt=0;
    unsigned int total=0;
    for(int i=0;i<9;i++){
        if(!(mask&(1u<<i))) continue;
        char nm[16]; int ln=sys_vt_getname(i,nm,sizeof(nm));
        char *l=lab[cnt]; unsigned int w=0;
        l[w++]=' ';
        if(ln>0){ for(int j=0;nm[j]&&w<16;j++) l[w++]=nm[j]; }
        else    { l[w++]='V'; l[w++]='T'; l[w++]=(char)('1'+i); }
        l[w++]=' '; l[w]='\0';
        idx[cnt]=i; total+=w; cnt++;
        if(cnt>=9) break;
    }
    if(!cnt) return 0;

    unsigned int avail=hi-lo;
    unsigned int c=(avail>total)?(lo+(avail-total)/2u):lo;
    for(int k=0;k<cnt;k++){
        unsigned char clr=(idx[k]==(int)active)?SB_TAB_ACT:SB_CLR;
        for(unsigned int j=0; lab[k][j] && c<hi; j++)
            put_cell(cells,n,c++,row,lab[k][j],clr);
    }
    return 1;
}

static void draw(void)
{
    unsigned int size=(unsigned int)sys_term_size();
    unsigned int cols=(size>>16)&0xFFFFu;
    unsigned int row =size&0xFFFFu;        /* status row = drawable rows */
    if(cols<12)return;
    if(cols>SB_MAXCELLS) cols=SB_MAXCELLS;

    char left[SEC_MAX],right[SEC_MAX];
    build_section(g_left,left);
    build_section(g_right,right);

    tty_cell_t cells[SB_MAXCELLS]; unsigned int n=0;
    for(unsigned int c=0;c<cols;c++) put_cell(cells,&n,c,row,' ',SB_CLR);

    unsigned int ll=s_len(left);
    if(left[0])   put_str(cells,&n,1,row,left,SB_CLR);

    unsigned int rl=s_len(right);
    if(rl && cols>rl+1) put_str(cells,&n,cols-rl-1,row,right,SB_CLR);

    /* Centre window = the gap between the left and right sections, so the
     * tabs / centre widget never overpaint the resource readouts.  One cell
     * of padding on each side. */
    unsigned int lo=left[0] ? (1+ll+1) : 0;
    unsigned int hi=(rl && cols>rl+1) ? (cols-rl-1) : cols;
    if(hi>cols) hi=cols;
    if(lo>hi)   lo=hi;

    /* Center: the live VT tabs when requested (the makmux strip), else a
     * normal widget string. */
    if(sb_has(g_center,"tabs")){
        render_tabs(cells,&n,row,lo,hi);
    } else {
        char center[SEC_MAX]; build_section(g_center,center);
        unsigned int cl=s_len(center);
        unsigned int avail=hi-lo;
        if(cl && avail>cl){ unsigned int cc=lo+(avail-cl)/2u; put_str(cells,&n,cc,row,center,SB_CLR); }
    }

    sys_putch_at(cells,n);
}

/* Active VT slot -> /dev/ttyN number: root slot (9) is tty0, the nine user
 * slots 0..8 are tty1..tty9.  This is the per-tab config key. */
static int active_tty_num(void)
{
    unsigned int active=((unsigned int)sys_vt_state()>>16)&0xFFFFu;
    if(active==9u) return 0;            /* VTTY_ROOT_SLOT -> tty0 */
    return (int)active+1;              /* slot N -> tty(N+1) */
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
    int last_tty=-2;                    /* force a reload on the first frame */
    for(;;){
        /* Reload the layout when the active tab changes (so each VT shows its
         * own bar) and every ~3 s otherwise (picks up login + ~/.sbrc edits). */
        unsigned int now=sys_uptime();
        int tty=active_tty_num();
        if(now>=rc_next || tty!=last_tty){
            if(sys_whoami(g_user,sizeof(g_user))<=0){ g_user[0]='u';g_user[1]='s';g_user[2]='e';g_user[3]='r';g_user[4]='\0'; }
            load_sbrc(tty);
            last_tty=tty;
            rc_next=now+300u;           /* 3 s at 100 Hz */
        }

        if(sys_statusbar_enabled())
            draw();

        unsigned int next=last+20u;     /* ~0.2 s frame */
        while(sys_uptime()<next) sys_yield();
        last=sys_uptime();
    }
}
