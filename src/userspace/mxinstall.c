/*
 * mxinstall.elf -- graphical OS installer, as a makx client.
 *
 * The GUI peer of the text-mode (TUI) installer: a step-by-step wizard that
 * collects the target drive, filesystem, hostname and accounts, then drives the
 * kernel's shared headless install engine (SYS_INSTALL_EXEC) one file at a time.
 * Because the copy is *stepped* -- one install_exec_step() per frame -- the
 * window repaints a live progress bar and the desktop/compositor keep running
 * between files, so the install never freezes the GUI the way a single blocking
 * install syscall would.
 *
 * The TUI installer (installer_run, run from a shell) and this client share the
 * exact same execution engine and install_params_t; only the front-end differs.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "gui_ui.h"
#include "makx.h"
#include <makar_abi.h>   /* install_params_t / _progress_t / _drive_t (shared ABI) */

#define RGB GFX_RGB
#define COL_BG     RGB(0x12,0x16,0x1e)
#define COL_PANEL  RGB(0x1b,0x22,0x2e)
#define COL_TITLE  RGB(0x40,0xc0,0xff)
#define COL_TEXT   RGB(0xe6,0xea,0xf0)
#define COL_MUTED  RGB(0x90,0xa0,0xb5)
#define COL_WARN   RGB(0xff,0x70,0x70)
#define COL_OK     RGB(0x60,0xff,0x80)
#define COL_BAR    RGB(0x4c,0x8d,0xff)

static void scpy(char *d,const char *s,int max){ int i=0; while(s[i]&&i<max-1){d[i]=s[i];i++;} d[i]=0; }
/* unsigned -> decimal, appended at *o in buf */
static void udec(char *buf,int *o,unsigned v){
    char t[12]; int n=0; if(!v){buf[(*o)++]='0';return;}
    while(v){t[n++]=(char)('0'+v%10);v/=10;}
    while(n) buf[(*o)++]=t[--n];
}

enum { ST_WELCOME=0, ST_DRIVE, ST_FS, ST_HOST, ST_ACCT, ST_CONFIRM, ST_RUN, ST_DONE, ST_FAIL };
enum { PH_PREP=0, PH_COPY, PH_FIN, PH_DONE, PH_FAIL };

int main(int argc, char **argv)
{
    mx_conn c;
    if (mx_connect(&c, argc, argv, 580, 460, MX_F_RESIZABLE) != 0) return 1;

    ui_ctx u; for (unsigned i=0;i<sizeof u/sizeof(int);i++) ((int*)&u)[i]=0;

    install_params_t  p;
    for (unsigned i=0;i<sizeof p;i++) ((unsigned char*)&p)[i]=0;
    p.fs = INSTALL_FS_EXT2; p.install_docs = 1; p.install_src = 1;
    scpy(p.hostname, "makar", sizeof p.hostname);

    install_drive_t drv[INSTALL_MAX_DRIVES];
    char drvlbl[INSTALL_MAX_DRIVES][64];
    const char *drvptr[INSTALL_MAX_DRIVES];
    int ndrv = sys_install_exec(3, drv);
    if (ndrv < 0) ndrv = 0;
    for (int i=0;i<ndrv;i++){
        int o=0; drvlbl[i][o++]='h'; drvlbl[i][o++]='d'; drvlbl[i][o++]=(char)('a'+i);
        drvlbl[i][o++]=' '; drvlbl[i][o++]='(';
        udec(drvlbl[i],&o,drv[i].size_mib);
        const char *mib=" MiB)  "; for(int k=0;mib[k];k++) drvlbl[i][o++]=mib[k];
        for(const char *m=drv[i].model; *m && o<62; m++) drvlbl[i][o++]=*m;
        drvlbl[i][o]=0; drvptr[i]=drvlbl[i];
    }
    int drv_sel=0, drv_scroll=0;
    if (ndrv>0) p.drive = drv[0].index;

    int step = ST_WELCOME, phase = PH_PREP, exec_rc = 0;
    install_progress_t prog; prog.done=0; prog.files=0; prog.current[0]=0;
    unsigned done_at = 0;
    int autologin_on = 0;
    int shown_step = -1;   /* drives auto-focus of the first field per step */

    while (!c.closed) {
        mx_pump(&c);
        int key = mx_key(&c);

        gfx_surface *s = &c.surf;
        int W = s->w, H = s->h;
        gfx_fill(s, 0, 0, W, H, COL_BG);
        gfx_fill(s, 0, 0, W, 30, COL_PANEL);
        gfx_str(s, 14, 11, "Install Makar OS", COL_TITLE);

        ui_begin(&u, c.mx, c.my, c.mdown, c.mpressed, c.mreleased, key);
        /* Immediate-mode focus is set by clicking a widget, so without this the
         * focus stays on whatever "Next" button got us here and typing goes
         * nowhere.  On entering a step, focus its first input (always widget id
         * 1 here -- listbox / textbox / first password).  Tab cycles fields. */
        if (step != shown_step) { u.focus = 1; shown_step = step; }
        if (key == '\t') { u.focus = u.focus + 1; if (u.focus > 3) u.focus = 1; }
        int by = H - 38, bnext = W - 104, bback = 14;
        int x = 20, y = 46;

        if (step == ST_WELCOME) {
            gfx_str(s, x, y,    "This wizard installs Makar OS to a hard disk.", COL_TEXT);
            gfx_str(s, x, y+20, "The target disk will be ERASED.", COL_WARN);
            gfx_str(s, x, y+48, "34 MiB FAT32 boot partition + a data partition", COL_MUTED);
            gfx_str(s, x, y+64, "(ext2 or FAT32) holding /apps, /docs, /src, /usr.", COL_MUTED);
            if (ndrv == 0)
                gfx_str(s, x, y+96, "No ATA target drive found - cannot install.", COL_WARN);
            if (ndrv > 0 && ui_button(&u,s,bnext,by,90,26,"Next >")) step = ST_DRIVE;
        }
        else if (step == ST_DRIVE) {
            gfx_str(s, x, y, "Select the target drive:", COL_TEXT);
            ui_listbox(&u, s, x, y+22, W-40, 120, drvptr, ndrv, &drv_sel, &drv_scroll);
            gfx_str(s, x, y+150, "WARNING: all data on the chosen drive is erased.", COL_WARN);
            if (ui_button(&u,s,bback,by,90,26,"< Back")) step = ST_WELCOME;
            if (ui_button(&u,s,bnext,by,90,26,"Next >")) {
                if (ndrv>0) p.drive = drv[drv_sel].index;
                step = ST_FS;
            }
        }
        else if (step == ST_FS) {
            gfx_str(s, x, y, "Data filesystem:", COL_TEXT);
            if (ui_button(&u,s,x,     y+22,150,26, p.fs==INSTALL_FS_EXT2 ? "[*] ext2" : "[ ] ext2"))
                p.fs = INSTALL_FS_EXT2;
            if (ui_button(&u,s,x+160, y+22,150,26, p.fs==INSTALL_FS_FAT32? "[*] FAT32": "[ ] FAT32"))
                p.fs = INSTALL_FS_FAT32;
            gfx_str(s, x, y+66, "Optional components:", COL_TEXT);
            if (ui_button(&u,s,x,     y+88,150,26, p.install_docs ? "[x] docs" : "[ ] docs"))
                p.install_docs = !p.install_docs;
            if (ui_button(&u,s,x+160, y+88,150,26, p.install_src  ? "[x] src"  : "[ ] src"))
                p.install_src = !p.install_src;
            if (ui_button(&u,s,bback,by,90,26,"< Back")) step = ST_DRIVE;
            if (ui_button(&u,s,bnext,by,90,26,"Next >")) step = ST_HOST;
        }
        else if (step == ST_HOST) {
            gfx_str(s, x, y, "Hostname:", COL_TEXT);
            ui_textbox(&u, s, x, y+22, 260, 26, p.hostname, sizeof p.hostname);
            gfx_str(s, x, y+58, "Letters, digits and hyphens.", COL_MUTED);
            if (ui_button(&u,s,bback,by,90,26,"< Back")) step = ST_FS;
            if (ui_button(&u,s,bnext,by,90,26,"Next >")) step = ST_ACCT;
        }
        else if (step == ST_ACCT) {
            gfx_str(s, x, y,    "Root password:", COL_TEXT);
            ui_password(&u, s, x+150, y, 200, 24, p.root_pw, sizeof p.root_pw);
            gfx_str(s, x, y+34, "New user (optional):", COL_TEXT);
            ui_textbox(&u, s, x+150, y+34, 200, 24, p.user_name, sizeof p.user_name);
            gfx_str(s, x, y+68, "User password:", COL_TEXT);
            ui_password(&u, s, x+150, y+68, 200, 24, p.user_pw, sizeof p.user_pw);
            if (ui_button(&u,s,x,y+104,210,26, autologin_on ? "[x] auto-login on boot" : "[ ] auto-login on boot"))
                autologin_on = !autologin_on;
            if (ui_button(&u,s,bback,by,90,26,"< Back")) step = ST_HOST;
            if (ui_button(&u,s,bnext,by,90,26,"Next >")) {
                /* Prefer the new user for auto-login, else root. */
                p.autologin[0]=0;
                if (autologin_on) {
                    if (p.user_name[0] && p.user_pw[0]) scpy(p.autologin, p.user_name, sizeof p.autologin);
                    else if (p.root_pw[0])              scpy(p.autologin, "root", sizeof p.autologin);
                }
                step = ST_CONFIRM;
            }
        }
        else if (step == ST_CONFIRM) {
            char line[96]; int o;
            gfx_str(s, x, y, "Ready to install:", COL_TEXT);
            o=0; { const char *t="  Target : "; for(int k=0;t[k];k++) line[o++]=t[k]; }
            { const char *m = (ndrv>0)? drvlbl[drv_sel] : "(none)"; for(int k=0;m[k]&&o<94;k++) line[o++]=m[k]; }
            line[o]=0; gfx_str(s, x, y+24, line, COL_MUTED);
            o=0; { const char *t="  Filesys: "; for(int k=0;t[k];k++) line[o++]=t[k]; }
            { const char *m = p.fs==INSTALL_FS_FAT32?"FAT32":"ext2"; for(int k=0;m[k];k++) line[o++]=m[k]; }
            line[o]=0; gfx_str(s, x, y+40, line, COL_MUTED);
            o=0; { const char *t="  Host   : "; for(int k=0;t[k];k++) line[o++]=t[k]; }
            { const char *m=p.hostname; for(int k=0;m[k]&&o<94;k++) line[o++]=m[k]; }
            line[o]=0; gfx_str(s, x, y+56, line, COL_MUTED);
            gfx_str(s, x, y+88, "This ERASES the target drive. This cannot be undone.", COL_WARN);
            if (ui_button(&u,s,bback,by,90,26,"< Back")) step = ST_CONFIRM, step = ST_ACCT;
            if (ui_button(&u,s,bnext-40,by,140,26,"Erase & Install")) { step = ST_RUN; phase = PH_PREP; }
        }
        else if (step == ST_RUN) {
            /* Draw the current status, present it, THEN do one unit of work --
             * so the "Preparing..." / current-file label is on screen during
             * the (brief) blocking begin/finish and during each file copy. */
            gfx_str(s, x, y, "Installing - please do not power off.", COL_TEXT);
            const char *pl = phase==PH_PREP ? "Preparing disk (partition + format)..."
                           : phase==PH_COPY ? "Copying files..."
                           : phase==PH_FIN  ? "Finishing..." : "";
            gfx_str(s, x, y+28, pl, COL_MUTED);
            /* progress: "<files>/<total> (P%)  <current>" + a real % bar. */
            if (phase==PH_COPY) {
                unsigned pct = prog.total ? (prog.files * 100u) / prog.total : 0u;
                char line[96]; int o=0;
                line[o++]=' '; line[o++]=' ';
                udec(line,&o,prog.files); line[o++]='/'; udec(line,&o,prog.total);
                line[o++]=' '; line[o++]='(';
                udec(line,&o,pct); line[o++]='%'; line[o++]=')'; line[o++]=' '; line[o++]=' ';
                for(const char *q=prog.current; *q && o<94; q++) line[o++]=*q;
                line[o]=0; gfx_str(s, x, y+50, line, COL_TEXT);
                int bw=W-40, bx=x, byb=y+76;
                gfx_fill(s,bx,byb,bw,14,COL_PANEL);
                int fillw = (int)((unsigned)(bw-2) * pct / 100u);
                gfx_fill(s,bx+1,byb+1,fillw,12,COL_BAR);
            }
            mx_present(&c);

            if (phase==PH_PREP) {
                exec_rc = sys_install_exec(0, &p);
                phase = (exec_rc < 0) ? PH_FAIL : PH_COPY;
                if (phase==PH_FAIL) step = ST_FAIL;
            } else if (phase==PH_COPY) {
                int r = sys_install_exec(1, &prog);
                if (r < 0) { exec_rc = r; phase = PH_FAIL; step = ST_FAIL; }
                else if (prog.done) phase = PH_FIN;
            } else if (phase==PH_FIN) {
                sys_install_exec(2, &p);
                phase = PH_DONE; step = ST_DONE; done_at = sys_uptime();
            }
            sys_yield();
            continue;   /* already presented this frame */
        }
        else if (step == ST_DONE) {
            (void)done_at;
            gfx_str(s, x, y,    "Installation complete!", COL_OK);
            gfx_str(s, x, y+24, "Remove the install media before rebooting.", COL_TEXT);
            gfx_str(s, x, y+44, "You can keep using the live session, or reboot", COL_MUTED);
            gfx_str(s, x, y+58, "into your newly installed system.", COL_MUTED);
            /* No auto-reboot: let the operator decide (Ubuntu-style) -- and on a
             * live CD keep the session up so they can see it finished. */
            if (ui_button(&u,s,bback,by,150,26,"Continue (live)")) break;
            if (ui_button(&u,s,bnext-20,by,120,26,"Reboot now"))    sys_reboot();
        }
        else if (step == ST_FAIL) {
            char line[64]; int o=0;
            const char *t="Installation failed (rc="; for(int k=0;t[k];k++) line[o++]=t[k];
            if (exec_rc<0){ line[o++]='-'; udec(line,&o,(unsigned)(-exec_rc)); }
            else udec(line,&o,(unsigned)exec_rc);
            line[o++]=')'; line[o]=0;
            gfx_str(s, x, y, line, COL_WARN);
            gfx_str(s, x, y+24, "Nothing was finalised. See the serial log.", COL_MUTED);
            if (ui_button(&u,s,bnext,by,90,26,"Quit")) break;
        }

        mx_present(&c);
        sys_yield();
    }
    mx_close(&c);
    return 0;
}
