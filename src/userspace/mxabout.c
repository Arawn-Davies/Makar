/*
 * mxabout.elf -- the "About Makar" makx client: copyright/identity + a live
 * system-spec panel (CPU, platform/hypervisor, RAM, display, disk, host/user,
 * uptime), read from /proc and the system-info syscalls.  A self-contained
 * makx client; the window's close button is server chrome.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "makx.h"

#define RGB GFX_RGB

static int  slen(const char *s){ int n=0; while(s[n])n++; return n; }
static void scpy(char *d,const char *s,int max){int i=0;while(s[i]&&i<max-1){d[i]=s[i];i++;}d[i]=0;}
static char *u2s(unsigned v,char *o){char t[12];int i=0;if(!v)t[i++]='0';while(v){t[i++]=(char)('0'+v%10u);v/=10u;}int j=0;while(i)o[j++]=t[--i];o[j]=0;return o;}

static int readfile(const char *p, char *b, int max)
{
    int fd = sys_open(p, O_RDONLY); if (fd < 0) { b[0]=0; return 0; }
    long n = sys_read(fd, b, max-1); sys_close(fd);
    if (n < 0) n = 0; b[n] = 0; return (int)n;
}

/* Copy the value after "<key>" (skipping a ':' and spaces) up to end-of-line. */
static void field(const char *buf, const char *key, char *out, int max)
{
    out[0] = 0;
    int kl = slen(key);
    for (const char *p = buf; *p; ) {
        int m = 0; while (m < kl && p[m] == key[m]) m++;
        if (m == kl) {
            const char *q = p + kl;
            while (*q == ' ' || *q == '\t' || *q == ':') q++;
            int i = 0; while (q[i] && q[i] != '\n' && i < max-1) { out[i] = q[i]; i++; }
            out[i] = 0; return;
        }
        while (*p && *p != '\n') p++; if (*p) p++;
    }
}

int main(int argc, char **argv)
{
    mx_conn c;
    if (mx_connect(&c, argc, argv, 560, 430) != 0) return 1;

    /* Gather specs once (RAM/disk/uptime refresh per redraw below). */
    static char cpuinfo[2048];
    readfile("/proc/cpuinfo", cpuinfo, sizeof cpuinfo);
    char vendor[32], hv[24]; field(cpuinfo, "vendor_id", vendor, sizeof vendor);
    field(cpuinfo, "hypervisor", hv, sizeof hv);
    if (!vendor[0]) scpy(vendor, "unknown", sizeof vendor);
    if (!hv[0])     scpy(hv, "bare metal", sizeof hv);
    char host[64] = {0}, user[64] = {0};
    sys_gethostname(host, sizeof host); sys_whoami(user, sizeof user);
    unsigned info = sys_fb_info();
    unsigned fbw = (info >> 16) & 0xFFFF, fbh = info & 0xFFFF;

    int first = 1, lfocus = -1;
    unsigned last_up = 0;
    while (!c.closed) {
        mx_pump(&c);
        (void)mx_key(&c);
        unsigned now = sys_uptime();
        int tick = (now - last_up) >= 100u;     /* refresh dynamic stats ~1 Hz */
        if (!(first || tick || c.focused != lfocus)) { sys_yield(); continue; }
        last_up = now; lfocus = c.focused; first = 0;

        char meminfo[512]; readfile("/proc/meminfo", meminfo, sizeof meminfo);
        char memtot[24]; field(meminfo, "MemTotal", memtot, sizeof memtot);
        unsigned tkb = 0, fkb = 0; sys_statfs(&tkb, &fkb);

        gfx_surface *s = &c.surf;
        gfx_fill(s, 0, 0, s->w, s->h, RGB(0x16,0x1b,0x24));
        gfx_fill(s, 0, 0, s->w, 2, RGB(0x35,0x6a,0xa8));

        int x = 18, y = 16;
        gfx_str(s, x, y, "Makar 0.9.5 -- The GCC/C++ sibling of Medli", RGB(0x8a,0xe2,0x34)); y += 16;
        gfx_str(s, x, y, "Copyright (C) 2026 Arawn Davies", RGB(0xd3,0xd7,0xcf)); y += 13;
        gfx_str(s, x, y, "Released under the BSD-3 Clause Clear license", RGB(0x90,0xa0,0xb5)); y += 13;
        gfx_str(s, x, y, "A hobby x86 (i386) bare-metal OS kernel in C + AT&T asm.", RGB(0x90,0xa0,0xb5)); y += 22;

        gfx_str(s, x, y, "System", RGB(0x4c,0x8d,0xff));
        gfx_fill(s, x, y+11, s->w-2*x, 1, RGB(0x2a,0x38,0x50)); y += 20;

        char line[128], num[24];
        #define ROW(label, val) do { scpy(line, label, sizeof line); int L=slen(line); \
            scpy(line+L, val, (int)sizeof line - L); \
            gfx_str(s, x, y, line, RGB(0xd3,0xd7,0xcf)); y += 15; } while (0)

        ROW("CPU        : ", vendor);
        ROW("Platform   : ", hv);
        { scpy(num, "", sizeof num); unsigned mb = 0;
          for (const char *q=memtot; *q>='0'&&*q<='9'; q++) mb = mb*10 + (unsigned)(*q-'0');
          char b[24]; u2s(mb/1024u, b); scpy(num, b, sizeof num); int L=slen(num); scpy(num+L," MB", (int)sizeof num-L); }
        ROW("Memory     : ", num);
        { char b[12],b2[12]; u2s(fbw,b); u2s(fbh,b2); scpy(num,b,sizeof num); int L=slen(num);
          num[L++]='x'; scpy(num+L,b2,(int)sizeof num-L); }
        ROW("Display    : ", num);
        { char b[16]; u2s((tkb>fkb?(tkb-fkb):0)/1024u, b); scpy(num,b,sizeof num); int L=slen(num);
          scpy(num+L," / ",(int)sizeof num-L); L=slen(num); char b2[16]; u2s(tkb/1024u,b2); scpy(num+L,b2,(int)sizeof num-L);
          L=slen(num); scpy(num+L," MB used",(int)sizeof num-L); }
        ROW("Disk       : ", num);
        ROW("Host       : ", host[0]?host:"makar");
        ROW("User       : ", user[0]?user:"user");
        { char b[16]; u2s(sys_uptime()/100u, b); scpy(num,b,sizeof num); int L=slen(num); scpy(num+L," s",(int)sizeof num-L); }
        ROW("Uptime     : ", num);
        #undef ROW

        mx_present(&c);
        sys_yield();
    }
    mx_close(&c);
    return 0;
}
