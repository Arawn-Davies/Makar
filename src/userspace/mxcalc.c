/*
 * mxcalc.elf -- a calculator, as a makx client.  A button grid + display over
 * a small integer expression evaluator (the same recursive-descent grammar as
 * the shell calc.elf: + - * / %, parens, unary).  Mouse or keyboard input.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "gui_ui.h"
#include "makx.h"

#define RGB GFX_RGB
#define COL_BG    RGB(0x12,0x16,0x1e)
#define COL_DISP  RGB(0x0a,0x0d,0x12)
#define COL_TEXT  RGB(0xd3,0xd7,0xcf)
#define COL_ERR   RGB(0xff,0x60,0x60)

static int slen(const char *s){ int n=0; while(s[n])n++; return n; }

/* ---- integer expression evaluator (no stdout; 0 ok / -1 error) ---------- */
static const char *p_; static int p_err;
static long e_expr(void);
static void e_ws(void){ while(*p_==' '||*p_=='\t') p_++; }
static long e_factor(void){
    e_ws();
    if(*p_=='('){ p_++; long v=e_expr(); e_ws(); if(*p_==')') p_++; else p_err=1; return v; }
    if(*p_<'0'||*p_>'9'){ p_err=1; return 0; }
    long v=0; while(*p_>='0'&&*p_<='9') v=v*10+(*p_++-'0'); return v;
}
static long e_unary(void){
    e_ws();
    if(*p_=='-'){ p_++; return -e_unary(); }
    if(*p_=='+'){ p_++; return  e_unary(); }
    return e_factor();
}
static long e_term(void){
    long v=e_unary();
    for(;;){ e_ws(); if(p_err) return 0; char op=*p_;
        if(op!='*'&&op!='/'&&op!='%') break;
        p_++; long r=e_unary(); if(p_err) return 0;
        if(op=='*') v*=r; else if(r==0){ p_err=1; return 0; }
        else if(op=='/') v/=r; else v%=r;
    }
    return v;
}
static long e_expr(void){
    long v=e_term();
    for(;;){ e_ws(); if(p_err) return 0; char op=*p_;
        if(op!='+'&&op!='-') break;
        p_++; long r=e_term(); if(p_err) return 0;
        if(op=='+') v+=r; else v-=r;
    }
    return v;
}
static int eval(const char *s, long *out){
    p_=s; p_err=0; long v=e_expr(); e_ws();
    if(*p_) p_err=1;
    if(p_err) return -1;
    *out=v; return 0;
}
static void ltoa(long v, char *b){
    char t[24]; int i=0; int neg = v<0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if(!u) t[i++]='0';
    while(u){ t[i++]=(char)('0'+u%10); u/=10; }
    int j=0; if(neg) b[j++]='-';
    while(i) b[j++]=t[--i];
    b[j]=0;
}

/* ---- input handling ----------------------------------------------------- */
static char expr[40];
static int  err;

static void append(char c){ int n=slen(expr); if(n<(int)sizeof expr-1){ expr[n]=c; expr[n+1]=0; err=0; } }
static void backspace(void){ int n=slen(expr); if(n){ expr[n-1]=0; err=0; } }
static void clear(void){ expr[0]=0; err=0; }
static void equals(void){
    if(!expr[0]) return;
    long r; if(eval(expr,&r)==0){ ltoa(r,expr); err=0; } else err=1;
}
static void feed(char c){
    if((c>='0'&&c<='9')||c=='+'||c=='-'||c=='*'||c=='/'||c=='%'||c=='('||c==')') append(c);
    else if(c=='='||c=='\n'||c=='\r') equals();
    else if(c=='\b'||c==127) backspace();
    else if(c=='c'||c=='C') clear();
}

int main(int argc, char **argv)
{
    mx_conn c;
    if(mx_connect(&c, argc, argv, 240, 300, MX_F_RESIZABLE)!=0) return 1;
    ui_ctx u; for(unsigned i=0;i<sizeof u/sizeof(int);i++)((int*)&u)[i]=0;

    /* 4-col x 5-row keypad. */
    static const char *labels[5][4] = {
        { "C", "<", "(", ")" },
        { "7", "8", "9", "/" },
        { "4", "5", "6", "*" },
        { "1", "2", "3", "-" },
        { "0", "=", "%", "+" },
    };
    int first=1, lmx=-1, lmy=-1;
    int g_menu=-1, g_about=0, g_quit=0;

    while(!c.closed && !g_quit){
        mx_pump(&c);
        int busy = (g_menu>=0) || g_about;
        int k, keyed=0;
        while((k=mx_key(&c))>=0){ if(!busy){ feed((char)k); keyed=1; } }

        int moved = (c.mx!=lmx||c.my!=lmy); lmx=c.mx; lmy=c.my;
        if(!(first||keyed||c.mpressed||c.mreleased||c.rpressed||moved||c.resized)){ sys_yield(); continue; }
        first=0;

        gfx_surface *s=&c.surf;
        gfx_fill(s,0,0,s->w,s->h,COL_BG);

        /* display (below the menu bar) */
        int top=UI_MENUBAR_H, dh=36, pad=6;
        gfx_fill(s,pad,top+pad,s->w-2*pad,dh,COL_DISP);
        const char *shown = expr[0] ? expr : "0";
        gfx_str(s,pad+8,top+pad+(dh-8)/2, shown, err?COL_ERR:COL_TEXT);

        /* keypad fills the area below the display */
        int gy=top+pad+dh+pad, gx=pad;
        int gw=s->w-2*pad, gh=s->h-gy-pad;
        int bw=gw/4, bh=gh/5;
        ui_begin(&u,c.mx,c.my,c.mdown,c.mpressed,c.mreleased,-1);
        ui_gate g; ui_gate_begin(&u,&g,busy);
        for(int r=0;r<5;r++) for(int col=0;col<4;col++){
            const char *lab=labels[r][col];
            if(ui_button(&u,s,gx+col*bw,gy+r*bh,bw-4,bh-4,lab)){
                char ch=lab[0];
                if(ch=='<') backspace();
                else feed(ch);
            }
        }
        ui_gate_end(&u,&g);
        static const char *al[]={"Makar calculator (mxcalc)","(c) 2026 Arawn Davies  --  MIT","","Integer expression evaluator (+ - * / %, parens).","Part of Makar OS."};
        ui_appbar(&u,s,"mxcalc",al,5,0,0,&g_menu,&g_about,&g_quit);
        mx_present(&c);
        sys_yield();
    }
    mx_close(&c);
    return 0;
}
