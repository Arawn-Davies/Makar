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
#include "doomkeys.h"
#include "doomgeneric.h"

#define KEYQUEUE_SIZE 32
static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_rd = 0, s_wr = 0;

static unsigned int  FB_W, FB_H, off_x, off_y;
static unsigned int *fullfb;

/* Windowed mode (launched by the window manager as `doom.elf -surface <id>`):
 * render into a shared surface the WM composites, take key bytes from stdin
 * (the WM forwards them) instead of locking the raw scancode stream, and never
 * call SYS_FB_PRESENT (the WM owns scanout).  See docs/gui.md. */
static int s_windowed = 0;
static int s_surface_id = -1;

/* stdin in windowed mode carries decoded key-*down* bytes only (no break
 * codes), so a held movement key would stick.  Synthesize a release a short
 * time after each press: tap-to-move.  Crude but playable for menus/turning. */
#define HOLD_TICS 12            /* ~120ms at 100Hz */
#define HELD_MAX  8
static struct { unsigned char key; unsigned int expire; } s_held[HELD_MAX];

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

/* Windowed mode: map a decoded key byte (ASCII or KEY_ARROW_* sentinel, as the
 * WM forwards) to a Doom key.  Returns 0 for keys we don't bind. */
static unsigned char convertWinKey(unsigned char a)
{
    switch (a) {
        case 13: case 10: return KEY_ENTER;
        case 27:          return KEY_ESCAPE;
        case 0x82:        return KEY_LEFTARROW;   /* KEY_ARROW_LEFT  */
        case 0x83:        return KEY_RIGHTARROW;  /* KEY_ARROW_RIGHT */
        case 0x80:        return KEY_UPARROW;     /* KEY_ARROW_UP    */
        case 0x81:        return KEY_DOWNARROW;   /* KEY_ARROW_DOWN  */
        case ' ':         return KEY_FIRE;
        case '\t':        return KEY_TAB;
        case 'e': case 'E': return KEY_USE;
        default:
            if (a >= '1' && a <= '9') return a;
            if (a >= 'a' && a <= 'z') return a;
            if (a >= 'A' && a <= 'Z') return (unsigned char)(a + 32);
            return 0;
    }
}

void DG_Init(void)
{
    if (s_windowed) {
        unsigned int info = sys_surface_info(s_surface_id);
        FB_W = (info >> 16) & 0xFFFF;
        FB_H = info & 0xFFFF;
        fullfb = (unsigned int *)sys_surface_map(s_surface_id);
        off_x = (FB_W > DOOMGENERIC_RESX) ? (FB_W - DOOMGENERIC_RESX) / 2 : 0;
        off_y = (FB_H > DOOMGENERIC_RESY) ? (FB_H - DOOMGENERIC_RESY) / 2 : 0;
        sys_fcntl(0, F_SETFL, O_NONBLOCK);   /* WM forwards keys on our stdin */
        return;
    }

    unsigned int info = sys_fb_info();
    FB_W = (info >> 16) & 0xFFFF;
    FB_H = info & 0xFFFF;

    fullfb = (unsigned int *)sys_mmap(0, (unsigned long)FB_W * FB_H * 4,
                                      PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    /* mmap zero-fills -> black letterbox borders, drawn once. */
    off_x = (FB_W > DOOMGENERIC_RESX) ? (FB_W - DOOMGENERIC_RESX) / 2 : 0;
    off_y = (FB_H > DOOMGENERIC_RESY) ? (FB_H - DOOMGENERIC_RESY) / 2 : 0;

    sys_keyboard_raw(2);                 /* scancode passthrough */
    sys_fcntl(0, F_SETFL, O_NONBLOCK);
    sys_statusbar_set(0);
}

static void handle_input(void)
{
    if (s_windowed) {
        unsigned int now = sys_uptime();
        /* expire held keys -> synthesize releases */
        for (int i = 0; i < HELD_MAX; i++)
            if (s_held[i].key && (int)(now - s_held[i].expire) >= 0) {
                kq_push(0, s_held[i].key);
                s_held[i].key = 0;
            }
        unsigned char a;
        while (sys_read(0, &a, 1) == 1) {
            unsigned char k = convertWinKey(a);
            if (!k) continue;
            kq_push(1, k);
            /* (re)arm an auto-release slot for this key */
            int slot = -1;
            for (int i = 0; i < HELD_MAX; i++) { if (s_held[i].key == k) { slot = i; break; } }
            if (slot < 0) for (int i = 0; i < HELD_MAX; i++) if (!s_held[i].key) { slot = i; break; }
            if (slot >= 0) { s_held[slot].key = k; s_held[slot].expire = now + HOLD_TICS; }
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
        /* Windowed: the WM composites the surface; never touch scanout. */
        if (!s_windowed)
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

/* If the caller didn't pass -iwad, look for a WAD next to doom.elf (/apps) or
 * in /tmp (where getwad.sh saves) and inject it.  Lets `doom` and the gui Doom
 * icon Just Work with a bundled/fetched WAD. */
static int has_iwad(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-iwad")) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    /* Pull a `-surface <id>` pair out of argv (window-manager launch) before
     * anything else, and strip it so doomgeneric never sees the unknown flag.
     * Without it doom runs its normal fullscreen path (shell `doom`). */
    static char *fa[18];
    int fc = 0;
    for (int i = 0; i < argc && fc < 17; i++) {
        if (i >= 1 && !strcmp(argv[i], "-surface") && i + 1 < argc) {
            const char *p = argv[i + 1];
            int v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
            s_surface_id = v; s_windowed = 1;
            i++;                 /* also skip the id */
            continue;
        }
        fa[fc++] = argv[i];
    }
    fa[fc] = 0;
    argv = fa; argc = fc;

    static char *na[16];
    if (!has_iwad(argc, argv)) {
        /* With -m 64 + a 40 MiB backed heap the full 12 MiB DOOM.WAD loads, so
         * prefer it; DOOM1.WAD (shareware) and /tmp fetches are fallbacks. */
        static const char *cand[] = {
            "/apps/DOOM.WAD",  "/tmp/DOOM.WAD",   "/apps/DOOM2.WAD",
            "/apps/DOOM1.WAD", "/apps/doom1.wad", "/tmp/DOOM1.WAD", 0
        };
        for (int i = 0; cand[i]; i++) {
            int fd = sys_open(cand[i], O_RDONLY);
            if (fd >= 0) {
                sys_close(fd);
                int n = 0;
                na[n++] = argv[0]; na[n++] = "-iwad"; na[n++] = (char *)cand[i];
                for (int j = 1; j < argc && n < 14; j++) na[n++] = argv[j];
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
