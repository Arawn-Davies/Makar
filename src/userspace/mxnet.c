/*
 * mxnet.elf -- network status, as a makx client.  Shows the eth0 / DHCP / DNS
 * state from SYS_NET_INFO and offers Renew / Release / Flush-DNS buttons
 * (SYS_NET_CTL) -- the GUI peer of the maknetcfg.elf shell tool.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "gui_ui.h"
#include "makx.h"

#define RGB GFX_RGB
#define COL_BG   RGB(0x12,0x16,0x1e)
#define COL_TEXT RGB(0xd3,0xd7,0xcf)
#define COL_HDR  RGB(0x8a,0xe2,0x34)

static char info[512];

static void refresh(void)
{
    int n = sys_net_info(info, (unsigned)sizeof info - 1);
    if (n < 0) n = 0;
    info[n] = '\0';
    if (!info[0]) {
        const char *m = "(no network info)";
        int i=0; for(; m[i]; i++) info[i]=m[i]; info[i]=0;
    }
}

int main(int argc, char **argv)
{
    mx_conn c;
    if (mx_connect(&c, argc, argv, 460, 320, MX_F_RESIZABLE) != 0) return 1;
    ui_ctx u; for(unsigned i=0;i<sizeof u/sizeof(int);i++)((int*)&u)[i]=0;
    refresh();
    int first=1, lmx=-1, lmy=-1;
    int g_menu=-1, g_about=0, g_quit=0;

    while (!c.closed && !g_quit) {
        mx_pump(&c);
        while (mx_key(&c) >= 0) { /* no keyboard actions */ }

        int moved=(c.mx!=lmx||c.my!=lmy); lmx=c.mx; lmy=c.my;
        if(!(first||c.mpressed||c.mreleased||c.rpressed||moved||c.resized)){ sys_yield(); continue; }
        first=0;

        gfx_surface *s=&c.surf;
        gfx_fill(s,0,0,s->w,s->h,COL_BG);

        int busy=(g_menu>=0)||g_about;
        ui_begin(&u,c.mx,c.my,c.mdown,c.mpressed,c.mreleased,-1);
        ui_gate g; ui_gate_begin(&u,&g,busy);
        int top=UI_MENUBAR_H, bx=8;
        if(ui_button(&u,s,bx,top+8,84,22,"Refresh")) refresh();
        bx+=90;
        if(ui_button(&u,s,bx,top+8,84,22,"Renew"))   { sys_net_ctl(NET_CTL_DHCP_RENEW);   refresh(); }
        bx+=90;
        if(ui_button(&u,s,bx,top+8,84,22,"Release")) { sys_net_ctl(NET_CTL_DHCP_RELEASE); refresh(); }
        bx+=90;
        if(ui_button(&u,s,bx,top+8,96,22,"Flush DNS")){ sys_net_ctl(NET_CTL_DNS_FLUSH);    refresh(); }
        ui_gate_end(&u,&g);

        /* Render the info text line by line. */
        int x=10, y=top+42;
        gfx_str(s,x,y,"Network:",COL_HDR); y+=14;
        for(const char *p=info; *p; ){
            char line[96]; int i=0;
            while(*p && *p!='\n' && i<(int)sizeof line-1) line[i++]=*p++;
            line[i]=0; if(*p=='\n') p++;
            if(y < s->h-10){ gfx_str_clip(s,x,y,line,COL_TEXT,s->w-10); y+=12; }
        }

        static const char *al[]={"Makar network status (mxnet)","(c) 2026 Arawn Davies  --  MIT","","eth0 / DHCP / DNS state, with renew / release / flush.","Part of Makar OS."};
        ui_appbar(&u,s,"mxnet",al,5,0,0,&g_menu,&g_about,&g_quit);
        mx_present(&c);
        sys_yield();
    }
    mx_close(&c);
    return 0;
}
