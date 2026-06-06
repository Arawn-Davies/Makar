/*
 * mxterm.elf -- terminal, as a makx client.  Forks /apps/sh.elf over a pair of
 * pipes, renders the byte stream as a character grid into its makx surface, and
 * forwards keys the display server delivers (over IPC) to the shell's stdin.
 * Was the W_TERMINAL window kind built into the old monolithic wm.c; now an
 * ordinary client process talking the makx protocol (makx.h).
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "makx.h"

#define RGB GFX_RGB
#define COL_TEXT RGB(0xd3,0xd7,0xcf)

static void scpy(char *d,const char *s,int max){int i=0;while(s[i]&&i<max-1){d[i]=s[i];i++;}d[i]=0;}
static char *scat(char *d,const char *s){while(*d)d++;while(*s)*d++=*s++;*d=0;return d;}

#define TCOLS 80
#define TROWS 50
static char term[TROWS][TCOLS];
static int  t_cols, t_rows, t_cr, t_cc;
static int  term_pid=-1, term_in=-1, term_out=-1;

static void term_clear(void){ for(int r=0;r<TROWS;r++)for(int c=0;c<TCOLS;c++)term[r][c]=' '; t_cr=t_cc=0; }
static void term_newline(void){ t_cc=0; if(++t_cr>=t_rows){ for(int r=0;r<t_rows-1;r++)for(int c=0;c<TCOLS;c++)term[r][c]=term[r+1][c]; for(int c=0;c<TCOLS;c++)term[t_rows-1][c]=' '; t_cr=t_rows-1; } }
static void term_putc(char ch){
    if(ch=='\n'){term_newline();return;}
    if(ch=='\r'){t_cc=0;return;}
    if(ch==8||ch==127){if(t_cc>0){t_cc--;term[t_cr][t_cc]=' ';}return;}
    if(ch<32)return;
    if(t_cc>=t_cols)term_newline();
    if(t_cr<TROWS&&t_cc<TCOLS)term[t_cr][t_cc++]=ch;
}
static void term_spawn(void){
    if(term_pid>0)return;
    int ip[2],op[2];
    if(sys_pipe(ip)<0||sys_pipe(op)<0){const char*m="terminal: pipe failed\n";for(int i=0;m[i];i++)term_putc(m[i]);return;}
    int pid=sys_fork();
    if(pid<0){const char*m="terminal: fork failed\n";for(int i=0;m[i];i++)term_putc(m[i]);return;}
    if(pid==0){
        sys_close(ip[1]);sys_close(op[0]);
        sys_dup2(ip[0],0);sys_dup2(op[1],1);sys_dup2(op[1],2);
        sys_close(ip[0]);sys_close(op[1]);
        char user[48],uarg[64];char*av[3]={"sh.elf",0,0};
        if(sys_whoami(user,sizeof user)>0){scpy(uarg,"--user=",sizeof uarg);scat(uarg,user);av[1]=uarg;}
        sys_execve("/apps/sh.elf",av,(char*const*)0);
        sys_exit(127);
    }
    term_pid=pid;term_in=ip[1];term_out=op[0];
    sys_close(ip[0]);sys_close(op[1]);
    sys_fcntl(term_out,F_SETFL,O_NONBLOCK);
    sys_fcntl(term_in,F_SETFL,O_NONBLOCK);
}
static void term_kill(void){
    if(term_pid>0){sys_kill(term_pid,SIGKILL);int st;sys_wait4(term_pid,&st,0);}
    if(term_in>=0)sys_close(term_in);
    if(term_out>=0)sys_close(term_out);
    term_pid=-1;term_in=-1;term_out=-1;
}
static int term_pump(void){
    if(term_out<0)return 0;
    unsigned char b[128];int dirty=0;
    for(;;){long n=sys_read(term_out,b,sizeof b);if(n<=0)break;for(long i=0;i<n;i++)term_putc((char)b[i]);dirty=1;if(n<(long)sizeof b)break;}
    if(term_pid>0){int st;if(sys_wait4(term_pid,&st,WNOHANG)==term_pid){term_pid=-1;const char*m="\n[shell exited]\n";for(int i=0;m[i];i++)term_putc(m[i]);dirty=1;}}
    return dirty;
}
static void term_key(int k){ if(term_in<0)return; unsigned char b=(unsigned char)k; if(b=='\r')b='\n'; sys_write(term_in,&b,1); }

static void term_draw(gfx_surface *s, int focused){
    gfx_fill(s,0,0,s->w,s->h,RGB(0x0e,0x12,0x18));
    t_cols=(s->w-8)/8; if(t_cols>TCOLS)t_cols=TCOLS;
    t_rows=(s->h-8)/8; if(t_rows>TROWS)t_rows=TROWS;
    for(int r=0;r<t_rows;r++)for(int c=0;c<t_cols;c++)
        if(term[r][c]!=' ')gfx_char(s,4+c*8,4+r*8,(unsigned char)term[r][c],COL_TEXT);
    if(focused && t_cr<t_rows && t_cc<t_cols)
        gfx_fill(s,4+t_cc*8,4+t_cr*8,8,8,RGB(0x8a,0xe2,0x34));
}

int main(int argc,char**argv){
    mx_conn c;
    if(mx_connect(&c,argc,argv,640,400,MX_F_RESIZABLE)!=0) return 1;
    /* Size the grid up front: term_putc wraps/scrolls using t_cols/t_rows, and
     * the shell can emit its banner+prompt before the first term_draw runs --
     * leaving these 0 would scroll the first output off into a negative row. */
    t_cols=(c.surf.w-8)/8; if(t_cols>TCOLS)t_cols=TCOLS; if(t_cols<1)t_cols=1;
    t_rows=(c.surf.h-8)/8; if(t_rows>TROWS)t_rows=TROWS; if(t_rows<1)t_rows=1;
    term_clear();
    term_spawn();
    int first=1;
    while(!c.closed){
        mx_pump(&c);
        int k,keyed=0; while((k=mx_key(&c))>=0){ term_key(k); keyed=1; }
        int dirty=term_pump();
        if(dirty||keyed||first||c.resized){ term_draw(&c.surf,c.focused); mx_present(&c); first=0; }
        sys_yield();
    }
    term_kill();
    mx_close(&c);
    return 0;
}
