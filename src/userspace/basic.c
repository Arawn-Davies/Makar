/*
 * basic.elf - a small C64-flavoured BASIC interpreter for Makar.
 *
 *   basic              interactive REPL (type a line number to store a
 *                      program line, RUN/LIST/NEW, or run a bare statement)
 *   basic prog.bas     load a .bas file, RUN it, then exit
 *
 * Integer-only by design: Makar doesn't init the x87 FPU or save/restore
 * it across context switches, so ring-3 float would corrupt under
 * preemption.  All arithmetic is 32-bit signed; fractional work (e.g. the
 * mandelbrot sample) uses fixed-point scaled integers.
 *
 * Graphics ride the existing SYS_DRAW_LINE (a 1px line is a point), so a
 * PLOT/LINE program can paint at native VESA resolution.  In VGA-text mode
 * the graphics statements report "?NO GRAPHICS".
 *
 * Supported: PRINT  LET/assign  IF..THEN  GOTO  GOSUB/RETURN  FOR..TO..STEP
 * ..NEXT  INPUT  REM  END/STOP  CLS  PLOT  LINE  COLOR  RUN  LIST  NEW.
 * Expressions: + - * / MOD, = <> < > <= >=, unary -, parentheses, ABS(),
 * RND(), SGN(); variables are 1-2 chars (letter then letter/digit).
 */

#include "syscall.h"

#define MAX_LINES   512
#define LINE_CAP    128
#define MAX_VARS     64
#define FOR_DEPTH    16
#define GOSUB_DEPTH  32

typedef struct { int num; char text[LINE_CAP]; } prog_line_t;

static prog_line_t g_prog[MAX_LINES];
static int         g_nlines;

typedef struct { char name[3]; int val; } var_t;
static var_t g_vars[MAX_VARS];
static int   g_nvars;

typedef struct {
    char name[3];     /* loop variable                    */
    int  limit;
    int  step;
    int  loop_pc;     /* line index of the FOR statement   */
    int  loop_off;    /* char offset to resume body from   */
} for_frame_t;
static for_frame_t g_for[FOR_DEPTH];
static int         g_for_sp;

typedef struct { int pc; int off; } gosub_frame_t;
static gosub_frame_t g_gosub[GOSUB_DEPTH];
static int           g_gosub_sp;

/* Execution cursor / control state. */
static int         g_pc;        /* current line index            */
static const char *g_cur;       /* parse cursor into current line */
static int         g_jumped;    /* a control statement set g_pc   */
static int         g_resume;    /* >=0: resume current line here  */
static int         g_stop;      /* END / STOP / error             */
static int         g_err;       /* 1 once an error was reported   */
static int         g_draw_rgb = 0xFFFFFF;  /* current COLOR        */

/* ---- tiny libc ----------------------------------------------------------- */

static unsigned int slen(const char *s){ unsigned int n=0; while(s[n])n++; return n; }
static void puts_(const char *s){ sys_write(1,s,slen(s)); }
static void putc_(char c){ sys_write(1,&c,1); }

static int is_digit(char c){ return c>='0'&&c<='9'; }
static int is_alpha(char c){ return (c>='A'&&c<='Z')||(c>='a'&&c<='z'); }
static char up(char c){ return (c>='a'&&c<='z') ? (char)(c-'a'+'A') : c; }

static void put_int(int v)
{
    char t[12]; int n=0; unsigned int u;
    if (v<0){ putc_('-'); u=(unsigned int)(-(long)v); } else u=(unsigned int)v;
    if (u==0){ putc_('0'); return; }
    while(u){ t[n++]=(char)('0'+u%10); u/=10; }
    while(n) putc_(t[--n]);
}

/* ---- error handling ------------------------------------------------------ */

static void basic_error(const char *msg)
{
    if (g_err) return;
    g_err = 1; g_stop = 1;
    puts_("?"); puts_(msg);
    if (g_pc >= 0 && g_pc < g_nlines) {
        puts_(" IN "); put_int(g_prog[g_pc].num);
    }
    putc_('\n');
}

/* ---- variables ----------------------------------------------------------- */

static void var_key(const char **pp, char out[3])
{
    const char *p = *pp;
    out[0]=up(*p++); out[1]=0; out[2]=0;
    if (is_alpha(*p) || is_digit(*p)) out[1]=up(*p++);
    /* skip any further name chars (BASIC keeps only first two). */
    while (is_alpha(*p) || is_digit(*p)) p++;
    *pp = p;
}

static int *var_slot(const char name[3])
{
    for (int i=0;i<g_nvars;i++)
        if (g_vars[i].name[0]==name[0] && g_vars[i].name[1]==name[1])
            return &g_vars[i].val;
    if (g_nvars < MAX_VARS) {
        g_vars[g_nvars].name[0]=name[0];
        g_vars[g_nvars].name[1]=name[1];
        g_vars[g_nvars].val=0;
        return &g_vars[g_nvars++].val;
    }
    return 0;
}

/* ---- expression evaluator (recursive descent) ---------------------------- */

static unsigned int g_rng = 0x1234abcdu;
static int rnd(int n)
{
    g_rng = g_rng*1103515245u + 12345u;
    int r = (int)((g_rng>>8) & 0x7FFFFFFF);
    if (n<=0) return r & 0x7FFF;
    return r % n;
}

static void skipsp(void){ while(*g_cur==' '||*g_cur=='\t') g_cur++; }

/* Match a keyword (case-insensitive) at the cursor; advance + return 1 on hit. */
static int kw(const char *k)
{
    skipsp();
    const char *p = g_cur;
    while (*k) { if (up(*p) != *k) return 0; p++; k++; }
    g_cur = p;
    return 1;
}

static int expr(void);   /* full expression incl. AND/OR (lowest prec) */
static int rel(void);    /* relational level                           */
static int fb_w(void);
static int fb_h(void);

static int primary(void)
{
    skipsp();
    char c = *g_cur;
    if (c=='(') { g_cur++; int v=expr(); skipsp(); if(*g_cur==')')g_cur++; else basic_error("SYNTAX ERROR"); return v; }
    if (c=='-') { g_cur++; return -primary(); }
    if (c=='+') { g_cur++; return  primary(); }
    if (is_digit(c)) {
        int v=0; while(is_digit(*g_cur)){ v=v*10+(*g_cur-'0'); g_cur++; } return v;
    }
    /* functions / variables */
    if (is_alpha(c)) {
        if (kw("ABS")) { skipsp(); if(*g_cur=='('){g_cur++;} int v=expr(); skipsp(); if(*g_cur==')')g_cur++; return v<0?-v:v; }
        if (kw("SGN")) { skipsp(); if(*g_cur=='('){g_cur++;} int v=expr(); skipsp(); if(*g_cur==')')g_cur++; return v>0?1:(v<0?-1:0); }
        if (kw("RND")) { skipsp(); int v=0; if(*g_cur=='('){g_cur++; v=expr(); skipsp(); if(*g_cur==')')g_cur++;} return rnd(v); }
        /* Screen geometry so programs can fill the display at any
         * resolution (XMAX/YMAX = last drawable pixel column/row). */
        if (kw("XMAX")) { int w=fb_w(); return w>0?w-1:0; }
        if (kw("YMAX")) { int h=fb_h(); return h>0?h-1:0; }
        char nm[3]; var_key(&g_cur, nm); int *s=var_slot(nm); return s?*s:0;
    }
    basic_error("SYNTAX ERROR");
    return 0;
}

static int term(void)
{
    int v = primary();
    for (;;) {
        skipsp();
        if (*g_cur=='*') { g_cur++; v *= primary(); }
        else if (*g_cur=='/') { g_cur++; int d=primary(); v = d? v/d : 0; }
        else if (kw("MOD")) { int d=primary(); v = d? v%d : 0; }
        else break;
    }
    return v;
}

static int addsub(void)
{
    int v = term();
    for (;;) {
        skipsp();
        if (*g_cur=='+') { g_cur++; v += term(); }
        else if (*g_cur=='-') { g_cur++; v -= term(); }
        else break;
    }
    return v;
}

/* Relational: returns -1 (true) / 0 (false), C64-style. */
static int rel(void)
{
    int l = addsub();
    skipsp();
    char a = *g_cur, b = g_cur[1];
    int op = 0;  /* 1:= 2:<> 3:< 4:> 5:<= 6:>= */
    if (a=='=') { op=1; g_cur+=1; }
    else if (a=='<' && b=='>') { op=2; g_cur+=2; }
    else if (a=='<' && b=='=') { op=5; g_cur+=2; }
    else if (a=='>' && b=='=') { op=6; g_cur+=2; }
    else if (a=='<') { op=3; g_cur+=1; }
    else if (a=='>') { op=4; g_cur+=1; }
    if (!op) return l;
    int r = addsub();
    int t;
    switch(op){
        case 1: t=(l==r); break; case 2: t=(l!=r); break;
        case 3: t=(l<r);  break; case 4: t=(l>r);  break;
        case 5: t=(l<=r); break; default: t=(l>=r); break;
    }
    return t ? -1 : 0;
}

/* AND / OR fold at the lowest precedence (bitwise on the -1/0 truth
 * values, C64-style, so they double as logical operators). */
static int expr(void)
{
    int v = rel();
    for (;;) {
        if (kw("AND"))      v &= rel();
        else if (kw("OR"))  v |= rel();
        else break;
    }
    return v;
}

/* ---- program management -------------------------------------------------- */

static int find_line(int num)
{
    for (int i=0;i<g_nlines;i++) if (g_prog[i].num==num) return i;
    return -1;
}

static void store_line(int num, const char *body)
{
    /* empty body => delete */
    int empty = 1; for (const char *q=body; *q; q++) if (*q!=' '&&*q!='\t'){empty=0;break;}
    int idx = find_line(num);
    if (empty) {
        if (idx>=0) { for (int i=idx;i<g_nlines-1;i++) g_prog[i]=g_prog[i+1]; g_nlines--; }
        return;
    }
    if (idx<0) {
        if (g_nlines>=MAX_LINES) return;
        /* insert sorted */
        int pos=g_nlines; for (int i=0;i<g_nlines;i++) if (g_prog[i].num>num){pos=i;break;}
        for (int i=g_nlines;i>pos;i--) g_prog[i]=g_prog[i-1];
        g_prog[pos].num=num; idx=pos; g_nlines++;
    }
    int o=0; while(body[o] && o<LINE_CAP-1){ g_prog[idx].text[o]=body[o]; o++; }
    g_prog[idx].text[o]='\0';
}

static void cmd_list(void)
{
    for (int i=0;i<g_nlines;i++){ put_int(g_prog[i].num); putc_(' '); puts_(g_prog[i].text); putc_('\n'); }
}

/* ---- statement execution ------------------------------------------------- */

static int fb_w(void){ return (int)sys_fb_width(); }

/* Drawable pixel height EXCLUDING the bottom makmux status row, so a
 * program that fills 0..YMAX leaves the status bar intact (and isn't
 * silently clipped by the kernel's draw-area clamp).  sys_term_rows()
 * reports the cell rows above the status bar; the status bar is one
 * more cell row, so cell_h = fb_h / (rows + 1). */
static int fb_h(void)
{
    int h = (int)sys_fb_height();
    if (h <= 0) return 0;
    int rows = (int)sys_term_rows();
    if (rows <= 0) return h;
    int cell_h = h / (rows + 1);
    return (cell_h > 0) ? (h - cell_h) : h;
}

/* 16-colour C64-ish palette → RGB. */
static const unsigned int PAL16[16] = {
    0x000000,0x0000AA,0x00AA00,0x00AAAA,0xAA0000,0xAA00AA,0xAA5500,0xAAAAAA,
    0x555555,0x5555FF,0x55FF55,0x55FFFF,0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF
};

static void stmt(void);

/* Run all ':'-separated statements from the cursor to end of line. */
static void exec_line(void)
{
    for (;;) {
        skipsp();
        if (*g_cur=='\0') return;
        stmt();
        if (g_stop || g_jumped) return;
        skipsp();
        if (*g_cur==':') { g_cur++; continue; }
        if (*g_cur=='\0') return;
        /* trailing garbage after a complete statement */
        return;
    }
}

static void do_print(void)
{
    int newline = 1;
    for (;;) {
        skipsp();
        char c=*g_cur;
        if (c=='\0' || c==':') break;
        if (c=='"') {
            g_cur++;
            while (*g_cur && *g_cur!='"') putc_(*g_cur++);
            if (*g_cur=='"') g_cur++;
        } else if (c==';') {
            g_cur++; newline=0;
        } else if (c==',') {
            g_cur++; putc_(' '); putc_(' '); newline=0;
        } else {
            int v=expr(); put_int(v); newline=1;
        }
        skipsp();
        if (*g_cur==';'||*g_cur==',') { newline=0; continue; }
        if (*g_cur=='\0'||*g_cur==':') break;
    }
    if (newline) putc_('\n');
}

static void do_input(void)
{
    skipsp();
    /* optional prompt string */
    if (*g_cur=='"') { g_cur++; while(*g_cur&&*g_cur!='"')putc_(*g_cur++); if(*g_cur=='"')g_cur++; skipsp(); if(*g_cur==';'||*g_cur==',')g_cur++; }
    else puts_("? ");
    skipsp();
    char nm[3]; var_key(&g_cur, nm); int *s=var_slot(nm);
    char buf[64]; long n=sys_read(0,buf,sizeof(buf)-1);
    int v=0,i=0,neg=0; if(n>0){ while(buf[i]==' ')i++; if(buf[i]=='-'){neg=1;i++;} while(i<n&&is_digit(buf[i])){v=v*10+(buf[i]-'0');i++;} }
    if (s) *s = neg?-v:v;
}

static void do_goto(void)
{
    int n=expr(); int idx=find_line(n);
    if (idx<0){ basic_error("UNDEF'D STATEMENT"); return; }
    g_pc=idx; g_resume=-1; g_jumped=1;
}

static void do_gosub(void)
{
    int n=expr(); int idx=find_line(n);
    if (idx<0){ basic_error("UNDEF'D STATEMENT"); return; }
    if (g_gosub_sp>=GOSUB_DEPTH){ basic_error("OUT OF MEMORY"); return; }
    g_gosub[g_gosub_sp].pc=g_pc;
    g_gosub[g_gosub_sp].off=(int)(g_cur - g_prog[g_pc].text);
    g_gosub_sp++;
    g_pc=idx; g_resume=-1; g_jumped=1;
}

static void do_return(void)
{
    if (g_gosub_sp<=0){ basic_error("RETURN WITHOUT GOSUB"); return; }
    g_gosub_sp--;
    g_pc=g_gosub[g_gosub_sp].pc; g_resume=g_gosub[g_gosub_sp].off; g_jumped=1;
}

static void do_for(void)
{
    skipsp();
    char nm[3]; var_key(&g_cur,nm); int *s=var_slot(nm);
    skipsp(); if(*g_cur=='=')g_cur++; int start=expr();
    if (!kw("TO")) { basic_error("SYNTAX ERROR"); return; }
    int limit=expr();
    int step=1; if (kw("STEP")) step=expr();
    if (s) *s=start;
    if (g_for_sp>=FOR_DEPTH){ basic_error("OUT OF MEMORY"); return; }
    for_frame_t *f=&g_for[g_for_sp++];
    f->name[0]=nm[0]; f->name[1]=nm[1]; f->name[2]=0;
    f->limit=limit; f->step=step;
    f->loop_pc=g_pc; f->loop_off=(int)(g_cur - g_prog[g_pc].text);
}

static void do_next(void)
{
    /* optional variable name (ignored for matching beyond the top frame) */
    skipsp(); if (is_alpha(*g_cur)) { char nm[3]; var_key(&g_cur,nm); }
    if (g_for_sp<=0){ basic_error("NEXT WITHOUT FOR"); return; }
    for_frame_t *f=&g_for[g_for_sp-1];
    int *s=var_slot(f->name); if(!s) return;
    *s += f->step;
    int done = (f->step>=0) ? (*s > f->limit) : (*s < f->limit);
    if (done) { g_for_sp--; return; }
    g_pc=f->loop_pc; g_resume=f->loop_off; g_jumped=1;
}

static void do_if(void)
{
    int cond=expr();
    if (!kw("THEN")) { basic_error("SYNTAX ERROR"); return; }
    if (cond) {
        skipsp();
        if (is_digit(*g_cur)) { do_goto(); }   /* IF .. THEN <line> */
        /* else fall through: rest of line executes as statements */
    } else {
        /* condition false: skip the remainder of the line */
        while (*g_cur) g_cur++;
    }
}

static void need_gfx_args(int *a, int n)
{
    for (int i=0;i<n;i++){ a[i]=expr(); skipsp(); if(*g_cur==',')g_cur++; }
}

static void do_plot(void)
{
    if (fb_w()<=0){ basic_error("NO GRAPHICS"); return; }
    int a[3]={0,0,-1}; a[2]=-1;
    a[0]=expr(); skipsp(); if(*g_cur==',')g_cur++;
    a[1]=expr(); skipsp();
    int rgb=g_draw_rgb;
    if(*g_cur==','){ g_cur++; int c=expr(); rgb = (c>=0&&c<16)?PAL16[c]:(unsigned)c; }
    sys_draw_line(a[0],a[1],a[0],a[1],(unsigned)rgb);
}

static void do_line(void)
{
    if (fb_w()<=0){ basic_error("NO GRAPHICS"); return; }
    int a[4]; need_gfx_args(a,4);
    int rgb=g_draw_rgb;
    skipsp(); if(*g_cur==','){ g_cur++; int c=expr(); rgb=(c>=0&&c<16)?PAL16[c]:(unsigned)c; }
    sys_draw_line(a[0],a[1],a[2],a[3],(unsigned)rgb);
}

static void do_color(void)
{
    int c=expr(); g_draw_rgb = (c>=0&&c<16)?PAL16[c]:(unsigned)c;
}

/* RECT x0,y0,x1,y1[,c] - filled rectangle (one native hline per row). */
static void do_rect(void)
{
    if (fb_w()<=0){ basic_error("NO GRAPHICS"); return; }
    int a[4]; need_gfx_args(a,4);
    int rgb=g_draw_rgb;
    skipsp(); if(*g_cur==','){ g_cur++; int c=expr(); rgb=(c>=0&&c<16)?PAL16[c]:(unsigned)c; }
    int y0=a[1], y1=a[3];
    if (y0>y1){ int t=y0; y0=y1; y1=t; }
    for (int y=y0; y<=y1; y++)
        sys_draw_line(a[0],y,a[2],y,(unsigned)rgb);
}

static void stmt(void)
{
    skipsp();
    if (*g_cur=='\0') return;

    if (kw("REM"))   { while(*g_cur)g_cur++; return; }
    if (kw("PRINT")) { do_print(); return; }
    if (kw("?"))     { do_print(); return; }
    if (kw("INPUT")) { do_input(); return; }
    if (kw("IF"))    { do_if(); return; }
    if (kw("GOTO"))  { do_goto(); return; }
    if (kw("GOSUB")) { do_gosub(); return; }
    if (kw("RETURN")){ do_return(); return; }
    if (kw("FOR"))   { do_for(); return; }
    if (kw("NEXT"))  { do_next(); return; }
    if (kw("END"))   { g_stop=1; return; }
    if (kw("STOP"))  { g_stop=1; return; }
    if (kw("CLS"))   { sys_tty_clear(VGA_CLR(VGA_LGREY,VGA_BLACK)); return; }
    if (kw("PLOT"))  { do_plot(); return; }
    if (kw("LINE"))  { do_line(); return; }
    if (kw("RECT"))  { do_rect(); return; }
    if (kw("COLOR")) { do_color(); return; }
    if (kw("LET"))   { /* fall through to assignment */ }

    /* assignment: VAR = expr */
    skipsp();
    if (is_alpha(*g_cur)) {
        const char *save=g_cur;
        char nm[3]; var_key(&g_cur,nm); skipsp();
        if (*g_cur=='=') { g_cur++; int v=expr(); int *s=var_slot(nm); if(s)*s=v; return; }
        g_cur=save;
    }
    basic_error("SYNTAX ERROR");
}

static void run_program(void)
{
    g_for_sp=0; g_gosub_sp=0; g_stop=0; g_err=0;
    g_pc=0; g_resume=-1;
    /* yield budget so a tight BASIC loop still cooperates with the scheduler */
    int budget=0;
    while (g_pc>=0 && g_pc<g_nlines && !g_stop) {
        g_cur = g_prog[g_pc].text + (g_resume>=0 ? g_resume : 0);
        g_resume=-1; g_jumped=0;
        exec_line();
        if (!g_jumped && !g_stop) g_pc++;
        if ((++budget & 0x3FF)==0) sys_yield();
    }
}

/* ---- REPL + file loader -------------------------------------------------- */

static int parse_leading_int(const char **pp)
{
    const char *p=*pp; while(*p==' ')p++;
    if (!is_digit(*p)) return -1;
    int v=0; while(is_digit(*p)){ v=v*10+(*p-'0'); p++; }
    *pp=p; return v;
}

/* Execute one immediate (non-numbered) line of input. */
static void immediate(char *line)
{
    /* trim */
    char *p=line;
    /* RUN / LIST / NEW recognised before falling into statement exec */
    while(*p==' ')p++;
    if (up(p[0])=='R'&&up(p[1])=='U'&&up(p[2])=='N') { run_program(); return; }
    if (up(p[0])=='L'&&up(p[1])=='I'&&up(p[2])=='S'&&up(p[3])=='T') { cmd_list(); return; }
    if (up(p[0])=='N'&&up(p[1])=='E'&&up(p[2])=='W') { g_nlines=0; g_nvars=0; return; }

    /* run as a one-shot statement on a synthetic line 0 */
    static char tmp[LINE_CAP];
    int o=0; while(p[o] && o<LINE_CAP-1){ tmp[o]=p[o]; o++; } tmp[o]='\0';
    g_pc=-1; g_cur=tmp; g_jumped=0; g_stop=0; g_err=0;
    g_for_sp=0; g_gosub_sp=0;
    exec_line();
}

static void feed_line(char *line)
{
    const char *p=line;
    int num=parse_leading_int(&p);
    if (num>=0) { while(*p==' ')p++; store_line(num,p); }
    else        immediate(line);
}

static char g_filebuf[64*1024];

static int load_and_run(const char *path)
{
    int fd=sys_open(path,O_RDONLY);
    if (fd<0){ puts_("basic: cannot open "); puts_(path); putc_('\n'); return 1; }
    long n=sys_read(fd,g_filebuf,sizeof(g_filebuf)-1);
    sys_close(fd);
    if (n<=0){ puts_("basic: empty file\n"); return 1; }
    g_filebuf[n]='\0';
    /* split into lines and feed each (store program lines). */
    int i=0; char ln[LINE_CAP];
    while (i<n) {
        int o=0; while(i<n && g_filebuf[i]!='\n'){ if(o<LINE_CAP-1) ln[o++]=g_filebuf[i]; i++; }
        ln[o]='\0'; i++;
        const char *p=ln; int num=parse_leading_int(&p);
        if (num>=0){ while(*p==' ')p++; store_line(num,p); }
        else if (o>0) { /* allow bare statements / RUN in a file */ char c=up(ln[0]);
            if (c=='R'||c=='L'||c=='N'||c=='P'||c=='?'||is_alpha(ln[0])) feed_line(ln); }
    }
    run_program();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc>=2) return load_and_run(argv[1]);

    puts_("Makar BASIC\n");
    puts_("READY.\n");
    static char line[LINE_CAP*2];
    for (;;) {
        long n=sys_read(0,line,sizeof(line)-1);
        if (n<=0) continue;
        /* strip newline */
        int len=(int)n; while(len>0 && (line[len-1]=='\n'||line[len-1]=='\r')) len--;
        line[len]='\0';
        /* EXIT / QUIT / BYE leave the interpreter */
        char c0=up(line[0]),c1=up(line[1]);
        if ((c0=='E'&&c1=='X') || (c0=='B'&&c1=='Y') || (c0=='Q'&&c1=='U')) break;
        feed_line(line);
        if (!g_err) puts_("READY.\n");
    }
    return 0;
}
