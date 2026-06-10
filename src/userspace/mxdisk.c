/*
 * mxdisk.elf -- disk / partition info, as a makx client.  Read-only view of
 * SYS_DISK_INFO (drives, partition table, FAT32 BPB) -- the GUI peer of
 * diskinfo.elf.  Destructive partitioning stays in cfdisk/fdisk.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "gui_ui.h"
#include "makx.h"

#define RGB GFX_RGB
#define COL_BG   RGB(0x12,0x16,0x1e)
#define COL_TEXT RGB(0xd3,0xd7,0xcf)
#define COL_HDR  RGB(0xf0,0xa8,0x30)

static char info[1024];

static void refresh(void)
{
    int n = sys_disk_info(info, (unsigned)sizeof info - 1);
    if (n < 0) n = 0;
    info[n] = '\0';
    if (!info[0]) {
        const char *m = "(no disk info)";
        int i=0; for(; m[i]; i++) info[i]=m[i]; info[i]=0;
    }
}

int main(int argc, char **argv)
{
    mx_conn c;
    if (mx_connect(&c, argc, argv, 520, 360, MX_F_RESIZABLE) != 0) return 1;
    ui_ctx u; for(unsigned i=0;i<sizeof u/sizeof(int);i++)((int*)&u)[i]=0;
    refresh();
    int first=1, lmx=-1, lmy=-1;
    int g_menu=-1, g_about=0, g_quit=0;

    while (!c.closed && !g_quit) {
        mx_pump(&c);
        while (mx_key(&c) >= 0) { /* read-only */ }

        int moved=(c.mx!=lmx||c.my!=lmy); lmx=c.mx; lmy=c.my;
        if(!(first||c.mpressed||c.mreleased||c.rpressed||moved||c.resized)){ sys_yield(); continue; }
        first=0;

        gfx_surface *s=&c.surf;
        gfx_fill(s,0,0,s->w,s->h,COL_BG);

        int busy=(g_menu>=0)||g_about;
        ui_begin(&u,c.mx,c.my,c.mdown,c.mpressed,c.mreleased,-1);
        ui_gate g; ui_gate_begin(&u,&g,busy);
        int top=UI_MENUBAR_H;
        if(ui_button(&u,s,8,top+8,84,22,"Refresh")) refresh();
        gfx_str(s,104,top+14,"Disks & partitions (read-only)",COL_HDR);
        ui_gate_end(&u,&g);

        int x=10, y=top+42;
        for(const char *p=info; *p; ){
            char line[120]; int i=0;
            while(*p && *p!='\n' && i<(int)sizeof line-1) line[i++]=*p++;
            line[i]=0; if(*p=='\n') p++;
            if(y < s->h-10){ gfx_str_clip(s,x,y,line,COL_TEXT,s->w-10); y+=12; }
        }

        static const char *al[]={"Makar disk info (mxdisk)","(c) 2026 Arawn Davies  --  MIT","","Read-only drives / partitions / FAT32 BPB.","Part of Makar OS."};
        ui_appbar(&u,s,"mxdisk",al,5,0,0,&g_menu,&g_about,&g_quit);
        mx_present(&c);
        sys_yield();
    }
    mx_close(&c);
    return 0;
}
