/*
 * mxterm.elf -- a real ANSI/VT100+ terminal, as a makx client.  Forks
 * /apps/sh.elf over a pair of pipes, feeds the child's byte stream through the
 * vt100 emulator core (vt100.c), and renders its colour cell grid into the makx
 * surface; forwards keys the display server delivers to the shell's stdin.
 *
 * Was a single-colour glass-TTY; now parses CSI/SGR/scroll-region/alt-screen so
 * TUI programs (maktop, vix, ls --color, ...) render correctly in the window.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "makx.h"
#include "vt100.h"

#define RGB GFX_RGB

static void scpy(char *d,const char *s,int max){int i=0;while(s[i]&&i<max-1){d[i]=s[i];i++;}d[i]=0;}
static char *scat(char *d,const char *s){while(*d)d++;while(*s)*d++=*s++;*d=0;return d;}

/* Standard 16-colour ANSI palette (0-7 normal, 8-15 bright). */
static const gfx_u32 PAL[16] = {
    RGB(0x00,0x00,0x00), RGB(0xaa,0x00,0x00), RGB(0x00,0xaa,0x00), RGB(0xaa,0x55,0x00),
    RGB(0x00,0x00,0xaa), RGB(0xaa,0x00,0xaa), RGB(0x00,0xaa,0xaa), RGB(0xc0,0xc4,0xbe),
    RGB(0x55,0x55,0x55), RGB(0xff,0x55,0x55), RGB(0x55,0xff,0x55), RGB(0xff,0xff,0x55),
    RGB(0x55,0x55,0xff), RGB(0xff,0x55,0xff), RGB(0x55,0xff,0xff), RGB(0xff,0xff,0xff) };

static vt_term vt;
static int term_pid=-1, term_in=-1, term_out=-1;

static void feed(const char *s){ while(*s) vt_putc(&vt,(unsigned char)*s++); }

static void term_spawn(void){
    if(term_pid>0)return;
    int ip[2],op[2];
    if(sys_pipe(ip)<0||sys_pipe(op)<0){ feed("terminal: pipe failed\r\n"); return; }
    int pid=sys_fork();
    if(pid<0){ feed("terminal: fork failed\r\n"); return; }
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
    unsigned char b[256];int dirty=0;
    for(;;){long n=sys_read(term_out,b,sizeof b);if(n<=0)break;for(long i=0;i<n;i++)vt_putc(&vt,b[i]);dirty=1;if(n<(long)sizeof b)break;}
    if(term_pid>0){int st;if(sys_wait4(term_pid,&st,WNOHANG)==term_pid){term_pid=-1;feed("\r\n[shell exited]\r\n");dirty=1;}}
    return dirty;
}
static void term_key(int k){ if(term_in<0)return; unsigned char b=(unsigned char)k; if(b=='\r')b='\n'; sys_write(term_in,&b,1); }

/* Grid geometry: 8x8 glyphs with a 4px margin. */
static void term_fit(gfx_surface *s){
    int cols=(s->w-8)/8, rows=(s->h-8)/8;
    if(cols<1)cols=1; if(rows<1)rows=1;
    if(cols!=vt.cols||rows!=vt.rows) vt_resize(&vt,cols,rows);
}
static void term_draw(gfx_surface *s, int focused){
    gfx_fill(s,0,0,s->w,s->h,PAL[0]);
    for(int r=0;r<vt.rows;r++)for(int c=0;c<vt.cols;c++){
        vt_cell *cl=&vt.cell[r][c];
        int x=4+c*8, y=4+r*8;
        if(cl->bg) gfx_fill(s,x,y,8,8,PAL[cl->bg&15]);
        if(cl->ch!=' ') gfx_char(s,x,y,cl->ch,PAL[cl->fg&15]);
    }
    if(focused && vt.cursor_visible && vt.cx<vt.cols && vt.cy<vt.rows)
        gfx_fill(s,4+vt.cx*8,4+vt.cy*8,8,8,RGB(0x8a,0xe2,0x34));
}

int main(int argc,char**argv){
    mx_conn c;
    if(mx_connect(&c,argc,argv,640,400,MX_F_RESIZABLE)!=0) return 1;
    { int cols=(c.surf.w-8)/8, rows=(c.surf.h-8)/8; vt_init(&vt,cols,rows); }
    term_spawn();
    int first=1;
    while(!c.closed){
        mx_pump(&c);
        if(c.resized) term_fit(&c.surf);
        int k,keyed=0; while((k=mx_key(&c))>=0){ term_key(k); keyed=1; }
        int dirty=term_pump();
        if(dirty||keyed||first||c.resized){ term_draw(&c.surf,c.focused); mx_present(&c); first=0; }
        sys_yield();
    }
    term_kill();
    mx_close(&c);
    return 0;
}
