/*
 * mxdoom.elf -- a Doom95-style launcher, as a makx client.  Lets you pick the
 * IWAD, an optional PWAD, skill, episode/map and monster options, then forks
 * doom.elf with the matching -iwad/-file/-skill/-warp/-nomonsters/-fast/-respawn
 * args (a separate process -> its own window; the launcher closes on launch).
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "gui_ui.h"
#include "gui_browser.h"
#include "makx.h"

#define RGB GFX_RGB
#define COL_BG   RGB(0x0c,0x0e,0x12)
#define COL_HEAD RGB(0xc0,0x40,0x40)
#define COL_TEXT RGB(0xd3,0xd7,0xcf)
#define COL_MUTE RGB(0x90,0xa0,0xb5)

static void scpy(char *d,const char *s,int max){int i=0;while(s[i]&&i<max-1){d[i]=s[i];i++;}d[i]=0;}
static int  slen(const char*s){int n=0;while(s[n])n++;return n;}
static char *u2s(unsigned v,char*o){char t[12];int i=0;if(!v)t[i++]='0';while(v){t[i++]=(char)('0'+v%10);v/=10;}int j=0;while(i)o[j++]=t[--i];o[j]=0;return o;}

#define MAXIWAD 16
static char iwad_path[MAXIWAD][96];
static char iwad_name[MAXIWAD][40];
static int  niwad = 0;

/* collect *.wad / *.WAD from a directory into the IWAD list */
static void scan_dir(const char *dir)
{
    struct dirent de;
    for (unsigned i=0; niwad<MAXIWAD; i++){
        int rc=sys_readdir(dir,i,&de);
        if (rc!=1) break;
        if (de.d_type==DT_DIR) continue;
        int n=slen(de.d_name);
        if (n<5) continue;
        const char *e=de.d_name+n-4;
        if (!((e[1]=='w'||e[1]=='W')&&(e[2]=='a'||e[2]=='A')&&(e[3]=='d'||e[3]=='D')&&e[0]=='.')) continue;
        int p=0; for(const char*q=dir;*q;q++) iwad_path[niwad][p++]=*q; iwad_path[niwad][p++]='/';
        for(int k=0; de.d_name[k]&&p<95; k++) iwad_path[niwad][p++]=de.d_name[k]; iwad_path[niwad][p]=0;
        scpy(iwad_name[niwad], de.d_name, sizeof iwad_name[0]);
        niwad++;
    }
}

static const char *SKILL[5] = {
    "I'm too young to die","Hey, not too rough","Hurt me plenty",
    "Ultra-Violence","Nightmare!"
};

int main(int argc, char **argv)
{
    mx_conn c;
    if (mx_connect(&c, argc, argv, 460, 286, 0) != 0) return 1;
    scan_dir("/usr/share/games/doom");
    scan_dir("/apps");

    ui_ctx u; for(unsigned i=0;i<sizeof u/sizeof(int);i++)((int*)&u)[i]=0;
    browser brz; for(unsigned i=0;i<sizeof brz/sizeof(int);i++)((int*)&brz)[i]=0;
    int dlg=0;

    int sel=0, skill=2, ep=1, map=1, nomon=0, fast=0, respawn=0;
    static char pwad[256]={0};

    int first=1, lmx=-1,lmy=-1;
    while(!c.closed){
        mx_pump(&c);
        int kk; while((kk=mx_key(&c))>=0){ (void)kk; }
        int moved=(c.mx!=lmx||c.my!=lmy); lmx=c.mx; lmy=c.my;
        if(!(first||c.mpressed||c.mreleased||moved||c.resized)){ sys_yield(); continue; }
        first=0;

        gfx_surface *s=&c.surf;
        gfx_fill(s,0,0,s->w,s->h,COL_BG);
        gfx_str(s,14,10,"DOOM Launcher",COL_HEAD);
        gfx_str(s,150,12,"makx",COL_MUTE);

        ui_begin(&u,c.mx,c.my,c.mdown,c.mpressed,c.mreleased,-1);

        int lx=14, fx=120, fw=s->w-fx-14, y=34, rh=28;
        char buf[80];

        /* Game WAD (cycle) */
        gfx_str(s,lx,y+8,"Game WAD:",COL_TEXT);
        { int o=0; const char*nm = niwad? iwad_name[sel] : "(no WAD found)";
          buf[o++]='<'; buf[o++]=' '; for(const char*p=nm;*p&&o<70;p++)buf[o++]=*p; buf[o++]=' '; buf[o++]='>'; buf[o]=0; }
        if (ui_button(&u,s,fx,y,fw,22,buf) && niwad) sel=(sel+1)%niwad;
        y+=rh;

        /* Custom WAD (PWAD) */
        gfx_str(s,lx,y+8,"Custom WAD:",COL_TEXT);
        ui_textbox(&u,s,fx,y,fw-74,22,pwad,sizeof pwad);
        if (ui_button(&u,s,fx+fw-70,y,70,22,"Browse")){ scpy(brz.cwd,"/usr/share/games/doom",sizeof brz.cwd); brz.sel=brz.scroll=0; brz.loaded=0; br_load(&brz); dlg=1; }
        y+=rh;

        /* Skill (cycle) */
        gfx_str(s,lx,y+8,"Skill:",COL_TEXT);
        { int o=0; buf[o++]='<'; buf[o++]=' '; for(const char*p=SKILL[skill-1];*p&&o<70;p++)buf[o++]=*p; buf[o++]=' '; buf[o++]='>'; buf[o]=0; }
        if (ui_button(&u,s,fx,y,fw,22,buf)) skill=skill%5+1;
        y+=rh;

        /* Episode / Map (cycle) */
        gfx_str(s,lx,y+8,"Episode:",COL_TEXT);
        { char t[2]={(char)('0'+ep),0}; int o=0; buf[o++]='<';buf[o++]=' ';buf[o++]=t[0];buf[o++]=' ';buf[o++]='>';buf[o]=0; }
        if (ui_button(&u,s,fx,y,60,22,buf)) ep=ep%4+1;
        gfx_str(s,fx+72,y+8,"Map:",COL_TEXT);
        { char t[2]={(char)('0'+map),0}; int o=0; buf[o++]='<';buf[o++]=' ';buf[o++]=t[0];buf[o++]=' ';buf[o++]='>';buf[o]=0; }
        if (ui_button(&u,s,fx+108,y,60,22,buf)) map=map%9+1;
        y+=rh;

        /* Monster options (toggles) */
        gfx_str(s,lx,y+8,"Monsters:",COL_TEXT);
        { scpy(buf,nomon?"[x] No Monsters":"[ ] No Monsters",sizeof buf); if(ui_button(&u,s,fx,y,fw,22,buf)) nomon=!nomon; } y+=26;
        { scpy(buf,fast?"[x] Fast Monsters":"[ ] Fast Monsters",sizeof buf); if(ui_button(&u,s,fx,y,fw,22,buf)) fast=!fast; } y+=26;
        { scpy(buf,respawn?"[x] Respawn Monsters":"[ ] Respawn Monsters",sizeof buf); if(ui_button(&u,s,fx,y,fw,22,buf)) respawn=!respawn; } y+=32;

        /* New Game / Cancel */
        int newg = ui_button(&u,s,lx,y,120,26, niwad? "New Game":"(no WAD)");
        int canc = ui_button(&u,s,s->w-14-90,y,90,26,"Cancel");

        if(dlg){
            char full[256];
            int r=br_dialog(&brz,&u,s,14,30,s->w-28,s->h-30-44,1,(char*)0,0,full,sizeof full);
            if(r==1){ scpy(pwad,full,sizeof pwad); dlg=0; }
            else if(r==2){ dlg=0; }
        }

        if(canc){ mx_close(&c); return 0; }
        if(newg && niwad && !dlg){
            char pidb[12]; u2s((unsigned)c.server,pidb);
            char skb[4]={(char)('0'+skill),0}, epb[4]={(char)('0'+ep),0}, mpb[4]={(char)('0'+map),0};
            char *av[24]; int n=0;
            av[n++]="/apps/doom.elf"; av[n++]="-makx"; av[n++]=pidb;
            av[n++]="-iwad"; av[n++]=iwad_path[sel];
            if(pwad[0]){ av[n++]="-file"; av[n++]=pwad; }
            av[n++]="-skill"; av[n++]=skb;
            if(ep>1||map>1){ av[n++]="-warp"; av[n++]=epb; av[n++]=mpb; }
            if(nomon)   av[n++]="-nomonsters";
            if(fast)    av[n++]="-fast";
            if(respawn) av[n++]="-respawn";
            av[n]=0;
            /* Replace ourselves with doom (NOT fork): doom keeps the pid the WM
             * launched, so the WM still reaps it on exit and the window closes
             * cleanly (a forked, orphaned doom can never be reaped).  doom
             * re-HELLOs on the same pid -> the server reuses this window. */
            sys_execve("/apps/doom.elf", av, (char *const*)0);
            /* execve only returns on failure -> stay in the launcher */
        }

        mx_present(&c);
        sys_yield();
    }
    mx_close(&c);
    return 0;
}
