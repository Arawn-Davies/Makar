/* doomgeneric_makar.c -- Makar (ring-3) platform backend for doomgeneric.
 *
 * Strictly userspace: talks to the kernel only through the syscall ABI.
 *   frames -> SYS_FB_PRESENT (640x400 centred in a full-FB back buffer)
 *   input  -> SYS_KEYBOARD_RAW(2) scancode passthrough (make+break), read nb
 *   timing -> SYS_UPTIME (100 Hz -> ms*10)
 * Patterned on doomgeneric_soso.c.  See HANDOFF.md / CLAUDE.roadmap.md #34.
 */
#include "syscall.h"
#include "string.h"
#include "makx.h"
#include "doomkeys.h"
#include "doomgeneric.h"

#define KEYQUEUE_SIZE 32
static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_rd = 0, s_wr = 0;

static unsigned int  FB_W, FB_H, off_x, off_y;
static unsigned int *fullfb;

/* Windowed mode: Doom is a makx *client* (launched by the display server with
 * `-makx <server-pid>`).  It renders into a shared surface the server
 * composites and never calls SYS_FB_PRESENT -- only the server owns scanout.
 * It connects with MX_F_RAWKEYS, so while it holds focus the server forwards
 * the raw make/break scancode stream as MXEV_KEY values -- the same stream the
 * fullscreen path reads directly -- giving true key-up events (no synthesized
 * releases).  See makx.h / docs/gui.md.  Without -makx, Doom runs its normal
 * fullscreen path (shell `doom`). */
static int     s_windowed = 0;
static mx_conn s_mc;

static void kq_push(int pressed, unsigned char k)
{
    s_KeyQueue[s_wr] = (unsigned short)((pressed << 8) | k);
    s_wr = (s_wr + 1) % KEYQUEUE_SIZE;
}

/* set-1 scancode (e0 collapsed to low7) -> Doom key */
static unsigned char convertToDoomKey(unsigned char sc)
{
    switch (sc) {
        case 0x1C: return KEY_ENTER;        /* Enter        */
        case 0x01: return KEY_ESCAPE;       /* Esc          */
        case 0x4B: return KEY_LEFTARROW;    /* Left / KP4   */
        case 0x4D: return KEY_RIGHTARROW;   /* Right / KP6  */
        case 0x48: return KEY_UPARROW;      /* Up / KP8     */
        case 0x50: return KEY_DOWNARROW;    /* Down / KP2   */
        case 0x1D: return KEY_FIRE;         /* Ctrl = fire  */
        case 0x39: return KEY_USE;          /* Space = use  */
        case 0x38: return KEY_STRAFE_L;     /* Alt (strafe modifier in vanilla) */
        case 0x2A: case 0x36: return KEY_RSHIFT;  /* Shift = run */
        case 0x0F: return KEY_TAB;          /* Tab = map    */
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x2C: return 'z';
        case 0x2D: return 'x';
        case 0x15: return 'y';
        case 0x31: return 'n';
        /* number row 1..7 -> weapon select */
        case 0x02: return '1'; case 0x03: return '2'; case 0x04: return '3';
        case 0x05: return '4'; case 0x06: return '5'; case 0x07: return '6';
        case 0x08: return '7';
        default:   return 0;
    }
}

void DG_Init(void)
{
    if (s_windowed) {
        /* the makx surface was mapped by mx_connect() in main() */
        FB_W = (unsigned int)s_mc.surf.w;
        FB_H = (unsigned int)s_mc.surf.h;
        fullfb = (unsigned int *)s_mc.surf.px;
        off_x = (FB_W > DOOMGENERIC_RESX) ? (FB_W - DOOMGENERIC_RESX) / 2 : 0;
        off_y = (FB_H > DOOMGENERIC_RESY) ? (FB_H - DOOMGENERIC_RESY) / 2 : 0;
        return;
    }

    unsigned int info = sys_fb_info();
    FB_W = (info >> 16) & 0xFFFF;
    FB_H = info & 0xFFFF;
    if (!FB_W || !FB_H) {
        /* No VESA framebuffer in this console (pure VGA text mode): Doom can't
         * present graphics here, so bail loudly instead of running blind on a
         * zero-sized buffer. */
        static const char m[] =
            "doom: no graphics framebuffer here -- run from a VESA console.\n";
        sys_write(2, m, sizeof m - 1);
        sys_exit(1);
    }

    fullfb = (unsigned int *)sys_mmap(0, (unsigned long)FB_W * FB_H * 4,
                                      PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    /* Commit the whole buffer up front (and draw the black letterbox border).
     * The anonymous mmap is demand-paged, but we only ever redraw the centred
     * DOOM region each frame -- the margin pages would stay unfaulted, and
     * SYS_FB_PRESENT validates the *entire* framebuffer extent is mapped before
     * the kernel reads it, so an unfaulted margin made every present fail (-1)
     * and the game never appeared on a text console.  memset faults every page
     * in (zero = black) so the present always sees a fully mapped buffer. */
    if (fullfb && fullfb != (unsigned int *)-1)
        memset(fullfb, 0, (unsigned long)FB_W * FB_H * 4);
    off_x = (FB_W > DOOMGENERIC_RESX) ? (FB_W - DOOMGENERIC_RESX) / 2 : 0;
    off_y = (FB_H > DOOMGENERIC_RESY) ? (FB_H - DOOMGENERIC_RESY) / 2 : 0;

    sys_keyboard_raw(2);                 /* scancode passthrough */
    sys_fcntl(0, F_SETFL, O_NONBLOCK);
    sys_statusbar_set(0);
    /* No upfront present or stdout redirect: doomgeneric's WAD-load chatter
     * prints to the console as usual, then the game loop's DG_DrawFrame presents
     * the first frame and the framebuffer takes over the screen -- the classic
     * "text scrolls, then it switches to graphics" boot.  (The display gate now
     * lets a text-console app present without being the focused VT; see
     * SYS_FB_PRESENT.) */
}

static void handle_input(void)
{
    if (s_windowed) {
        /* The WM forwards the raw make/break scancode stream (MX_F_RAWKEYS), so
         * the windowed path is the fullscreen path with mx_key() as the source:
         * real key-up events, no synthesized releases. */
        mx_pump(&s_mc);
        if (s_mc.closed) sys_exit(0);        /* server closed our window */
        int a;
        while ((a = mx_key(&s_mc)) >= 0) {
            unsigned char sc = (unsigned char)a;
            int pressed = (sc & 0x80) ? 0 : 1;
            unsigned char k = convertToDoomKey(sc & 0x7F);
            if (!k) continue;
            kq_push(pressed, k);
        }
        return;
    }

    unsigned char sc;
    while (sys_read(0, &sc, 1) == 1) {
        int pressed = (sc & 0x80) ? 0 : 1;
        unsigned char k = convertToDoomKey(sc & 0x7F);
        if (!k) continue;
        kq_push(pressed, k);
    }
}

void DG_DrawFrame(void)
{
    if (fullfb && fullfb != (unsigned int *)-1) {
        for (unsigned int y = 0; y < DOOMGENERIC_RESY; y++)
            memcpy(fullfb + off_x + (off_y + y) * FB_W,
                   DG_ScreenBuffer + y * DOOMGENERIC_RESX,
                   DOOMGENERIC_RESX * 4);
        /* Windowed: push the frame to the server (it composites); never touch
         * scanout directly.  Fullscreen: present to the real framebuffer. */
        if (s_windowed)
            mx_present(&s_mc);
        else
            sys_fb_present(fullfb);
    }
    handle_input();
}

void DG_SleepMs(uint32_t ms)
{
    unsigned int target = sys_uptime() + (ms / 10u);
    while (sys_uptime() < target) sys_yield();
}

uint32_t DG_GetTicksMs(void)
{
    return sys_uptime() * 10u;           /* 100 Hz ticks -> ms */
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
    if (s_rd == s_wr) return 0;
    unsigned short d = s_KeyQueue[s_rd];
    s_rd = (s_rd + 1) % KEYQUEUE_SIZE;
    *pressed = d >> 8;
    *doomKey = d & 0xFF;
    return 1;
}

void DG_SetWindowTitle(const char *title) { (void)title; }

/* Flag handling (passed straight through to doomgeneric's d_main.c):
 *   doom -iwad /apps/DOOM.WAD            pick a specific IWAD
 *   doom -file /tmp/mymap.wad …          load one or more PWADs (mods/maps)
 *   doom -iwad <iwad> -file <pwad> …     both together
 * If no -iwad is given we auto-pick a bundled/fetched IWAD (cand[] below).
 * `-makx <pid>` (display-server handle) is stripped before doomgeneric sees it.
 *
 * If the caller didn't pass -iwad, look for a WAD in /apps (bundled) or /tmp
 * (where getwad.sh saves) and inject it. */
static int has_iwad(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-iwad")) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    /* If launched by the display server (`-makx <pid>` in argv), connect as a
     * makx client and request a 640x400 surface up front -- DG_Init then renders
     * into it.  Strip the flag so doomgeneric never sees it.  No -makx -> the
     * normal fullscreen path (shell `doom`). */
    if (mx_connect(&s_mc, argc, argv, DOOMGENERIC_RESX, DOOMGENERIC_RESY,
                   MX_F_RAWKEYS) == 0)
        s_windowed = 1;

    static char *fa[34];
    int fc = 0;
    for (int i = 0; i < argc && fc < 33; i++) {
        if (i >= 1 && !strcmp(argv[i], "-makx") && i + 1 < argc) { i++; continue; }
        fa[fc++] = argv[i];
    }
    fa[fc] = 0;
    argv = fa; argc = fc;

    static char *na[36];
    if (!has_iwad(argc, argv)) {
        /* Prefer the BSD-licensed FreeDOOM IWADs (shipped by default, see
         * getfreedoom.sh) so DOOM Just Works without the copyrighted DOOM.WAD;
         * fall back to DOOM.WAD / DOOM1.WAD (shareware) and /tmp fetches if
         * someone supplies them. */
        static const char *cand[] = {
            /* XFCE-style asset path (shipped + installed): /usr/share/games/doom */
            "/usr/share/games/doom/freedoom1.wad",
            "/usr/share/games/doom/freedoom2.wad",
            "/usr/share/games/doom/DOOM.WAD",
            "/usr/share/games/doom/DOOM2.WAD",
            "/usr/share/games/doom/DOOM1.WAD",
            "/usr/share/games/doom/doom1.wad",
            /* legacy / fallback locations (manual drops, /tmp fetches) */
            "/apps/freedoom1.wad", "/apps/freedoom2.wad",
            "/apps/DOOM.WAD",  "/tmp/DOOM.WAD",   "/apps/DOOM2.WAD",
            "/apps/DOOM1.WAD", "/apps/doom1.wad", "/tmp/DOOM1.WAD", 0
        };
        for (int i = 0; cand[i]; i++) {
            int fd = sys_open(cand[i], O_RDONLY);
            if (fd >= 0) {
                sys_close(fd);
                int n = 0;
                na[n++] = argv[0]; na[n++] = "-iwad"; na[n++] = (char *)cand[i];
                for (int j = 1; j < argc && n < 34; j++) na[n++] = argv[j];
                na[n] = 0;
                argv = na; argc = n;
                break;
            }
        }
    }
    doomgeneric_Create(argc, argv);
    for (;;)
        doomgeneric_Tick();
    return 0;
}
