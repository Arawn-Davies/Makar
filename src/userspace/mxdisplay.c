/*
 * mxdisplay.elf -- display settings: change the screen resolution with a
 * confirm + 15-second auto-revert safety net (the safe-monitor pattern).
 *
 * Apply calls SYS_SETMODE; the kernel repoints the framebuffer and the window
 * manager reflows in place (wm_reinit_display) -- no process restart / re-login.
 * After applying, a "Keep this resolution?" prompt counts down and reverts to
 * the previous mode if the user doesn't confirm, so an unsupported or garbled
 * mode can never lock them out.  A self-contained makx client.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "gui_ui.h"
#include "makx.h"

#define RGB GFX_RGB
#define COL_BG RGB(0x16,0x1b,0x24)
#define COL_TX RGB(0xd3,0xd7,0xcf)
#define COL_HD RGB(0x4c,0x8d,0xff)
#define COL_ERR RGB(0xff,0x70,0x70)

static void scpy(char *d,const char *s,int max){int i=0;while(s[i]&&i<max-1){d[i]=s[i];i++;}d[i]=0;}
static int  slen(const char *s){int n=0;while(s[n])n++;return n;}
static int  streq(const char *a,const char *b){int i=0;while(a[i]&&a[i]==b[i])i++;return a[i]==b[i];}
static char *u2s(unsigned v,char *o){char t[12];int i=0;if(!v)t[i++]='0';while(v){t[i++]=(char)('0'+v%10u);v/=10u;}int j=0;while(i)o[j++]=t[--i];o[j]=0;return o;}

typedef struct { const char *name; unsigned w, h; } dmode;
/* Names the kernel's admin_setmode accepts (vesa_modes[]); offered widest-first. */
static const dmode MODES[] = {
    { "640x480",   640,  480  },
    { "1280x720",  1280, 720  },
    { "1920x1080", 1920, 1080 },
};
#define NMODES ((int)(sizeof(MODES)/sizeof(MODES[0])))

/* Map the live geometry to one of our mode names (else "640x480" as a floor). */
static const char *mode_for(unsigned w, unsigned h)
{
    for (int i=0;i<NMODES;i++) if (MODES[i].w==w && MODES[i].h==h) return MODES[i].name;
    return MODES[0].name;
}

enum { ST_LIST, ST_CONFIRM };

int main(int argc, char **argv)
{
    mx_conn c;
    if (mx_connect(&c, argc, argv, 380, 300, 0) != 0) return 1;

    ui_ctx u; for(unsigned i=0;i<sizeof u/sizeof(int);i++)((int*)&u)[i]=0;

    char cur_mode[16];
    { unsigned info=sys_fb_info(); scpy(cur_mode, mode_for((info>>16)&0xFFFF, info&0xFFFF), sizeof cur_mode); }

    int st = ST_LIST;
    char prev_mode[16] = {0};       /* mode to revert to while confirming      */
    unsigned deadline = 0;          /* sys_uptime() tick when auto-revert fires */
    char msg[48] = {0};             /* transient status / error line           */

    int first=1, lmx=-1, lmy=-1; unsigned last=0;
    while (!c.closed) {
        mx_pump(&c);
        (void)mx_key(&c);
        unsigned now = sys_uptime();
        int moved = (c.mx!=lmx||c.my!=lmy); lmx=c.mx; lmy=c.my;
        int tick = (now-last) >= 25u;          /* ~4 Hz: drive the countdown */
        if (!(first||tick||moved||c.mpressed||c.mreleased||c.resized)) { sys_yield(); continue; }
        last=now; first=0;

        /* Auto-revert if the confirm window elapsed. */
        if (st==ST_CONFIRM && (int)(now - deadline) >= 0) {
            sys_setmode(prev_mode);
            scpy(cur_mode, prev_mode, sizeof cur_mode);
            scpy(msg, "Reverted (no confirmation).", sizeof msg);
            st = ST_LIST;
        }

        gfx_surface *s = &c.surf;
        gfx_fill(s,0,0,s->w,s->h,COL_BG);
        gfx_fill(s,0,0,s->w,2,RGB(0x35,0x6a,0xa8));
        gfx_str(s,16,14,"Display Settings",COL_HD);

        ui_begin(&u,c.mx,c.my,c.mdown,c.mpressed,c.mreleased,-1);

        if (st==ST_LIST) {
            gfx_str(s,16,40,"Resolution:",COL_TX);
            for (int i=0;i<NMODES;i++){
                int by=58+i*40;
                char lab[24]; scpy(lab,MODES[i].name,sizeof lab);
                int active = streq(cur_mode, MODES[i].name);
                if (ui_button(&u,s,16,by,200,30,lab)){
                    scpy(prev_mode,cur_mode,sizeof prev_mode);
                    if (sys_setmode(MODES[i].name)==0){
                        scpy(cur_mode,MODES[i].name,sizeof cur_mode);
                        deadline = now + 1500u;   /* 15 s at 100 Hz uptime */
                        msg[0]=0;
                        st=ST_CONFIRM;
                    } else {
                        scpy(msg,"Mode not supported by this adapter.",sizeof msg);
                    }
                }
                if (active) gfx_str(s,228,by+11,"(current)",RGB(0x8a,0xe2,0x34));
            }
            if (msg[0]) gfx_str(s,16,s->h-24,msg,COL_ERR);
        } else { /* ST_CONFIRM */
            int left = ((int)(deadline - now))/100; if(left<0) left=0;
            gfx_str(s,16,46,"Keep this resolution?",COL_TX);
            char line[48]; char nb[12];
            scpy(line,"Reverting in ",sizeof line); int L=slen(line);
            scpy(line+L,u2s((unsigned)left,nb),(int)sizeof line-L); L=slen(line);
            scpy(line+L,"s ...",(int)sizeof line-L);
            gfx_str(s,16,66,line,RGB(0x90,0xa0,0xb5));
            if (ui_button(&u,s,16,100,160,32,"Keep changes")){
                scpy(msg,"Resolution kept.",sizeof msg);
                st=ST_LIST;
            }
            if (ui_button(&u,s,188,100,160,32,"Revert now")){
                sys_setmode(prev_mode);
                scpy(cur_mode,prev_mode,sizeof cur_mode);
                scpy(msg,"Reverted.",sizeof msg);
                st=ST_LIST;
            }
        }

        mx_present(&c);
        sys_yield();
    }
    mx_close(&c);
    return 0;
}
