/*
 * installer.c - interactive OS-to-disk installer (TUI).
 *
 * Full-screen wizard that:
 *   1. lists the ATA target drives and lets the user pick one (arrow keys);
 *   2. asks for the data filesystem (ext2 or FAT32);
 *   3. partitions the disk: a 34 MiB FAT32 boot partition (kernel +
 *      limine-bios.sys + limine.conf) plus a data partition spanning the rest
 *      (ext2 or FAT32, per the user's choice) holding /apps, /docs, /src;
 *   4. formats both partitions and copies files onto them;
 *   5. installs limine: boot sector -> MBR (preserving the partition table),
 *      stage 2 -> post-MBR embedding gap, stage-2 location patched at 0x1a4;
 *   6. unmounts and reports success.
 *
 * At runtime the FAT32 boot partition mounts as /mnt/boot and the data
 * partition mounts as /mnt/root (auto-detected by vfs_auto_mount).
 *
 * Rendering is a real ANSI/VT100 terminal byte stream written to the invoking
 * task's stdout (kfd_stdout_write): on a live text VT the bytes reach the
 * framebuffer console's ANSI parser (vt_putchar); inside an mxterm window they
 * reach the client's vt100 emulator over a pipe -- so the installer runs both
 * from a shell and in a GUI window with one code path.  With no terminal at all
 * (no VESA, not piped) it degrades to a plain numbered-prompt flow.  The shell
 * repaints the screen (shell_restore_screen) after installer_run() returns.
 *
 * The host build still boots via GRUB; this deploys limine at runtime, reading
 * the vendored limine-bios.sys off the CD (staged at /limine/limine-bios.sys
 * by iso.sh).
 */

#include <kernel/installer.h>
#include <kernel/task.h>
#include <kernel/auth.h>
#include <kernel/acpi.h>
#include <kernel/timer.h>
#include <kernel/rtc.h>
#include <kernel/ide.h>
#include <kernel/iso9660.h>
#include <kernel/partition.h>
#include <kernel/fat32.h>
#include <kernel/ext2.h>
#include <kernel/vfs.h>
#include <kernel/fd.h>
#include <kernel/logfs.h>
#include <kernel/heap.h>
#include <kernel/tty.h>
#include <kernel/vesa_tty.h>
#include <kernel/serial.h>
#include <kernel/keyboard.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* Tunables / disk layout constants                                          */
/* ------------------------------------------------------------------------- */

#define INST_PART_START_LBA   2048u          /* 1 MiB aligned first partition */
/* Bootfs sized to "just enough": limine-bios.sys (~120 KiB) + kernel
 * (~650 KiB) + limine.conf + FAT32 reserved sectors + cluster overhead +
 * breathing room for multiple kernel versions.  34 MiB is generous (~10x
 * what limine+kernel actually occupy today).  Everything else on the disk
 * becomes rootfs (apps + src + docs + user data).  Tweak INST_BOOT_MB if
 * a future bootloader gets fatter. */
/* FAT32 requires >= 65525 clusters which at 512-byte sectors works out
 * to roughly 33 MiB minimum (the spec's "small FAT32" trigger).  mkfs
 * refuses anything below that, so 34 MiB / 16 MiB are not viable bootfs
 * sizes despite being more than enough by usage.  34 MiB is the
 * smallest reliable FAT32 + a touch of headroom. */
#define INST_BOOT_MB          34u
#define INST_BOOT_SECTORS     (INST_BOOT_MB * 2048u)   /* 34 MiB FAT32 boot partition \
                                              * (assumes 512-byte sectors)     */
/* Per-file copy buffer cap.  Must exceed the largest file we ship or the
 * installer silently truncates it (iso9660_read_file copies min(size, bufsz)),
 * which corrupts the tail — e.g. a truncated DOOM.WAD (~11.8 MiB) loses its
 * lump directory and DOOM fails with "W_GetNumForName: PNAMES not found!".
 * 32 MiB covers the FreeDOOM WADs (freedoom1 ~21 MiB, freedoom2 ~28 MiB) as
 * well as DOOM.WAD/DOOM2.WAD.  It's a transient kmalloc in the 40 MiB heap,
 * live only during the copy phase (mkfs metadata is block-sized), so it fits.
 * A larger asset would need true streaming (ISO offset-read + FS append) --
 * noted as a follow-up; the multi-sector FS writes already make the copy fast. */
#define INST_MAX_FILE_SIZE    (32u * 1024u * 1024u)
#define MBR_PART_TABLE_OFF    0x1BEu
#define MBR_SIG_OFF           0x1FEu
#define LIMINE_STAGE2_LOC_OFF 0x1A4u          /* u64 stage-2 byte offset       */
#define LIMINE_SYS_ISO_PATH   "/limine/limine-bios.sys"   /* stage3 -> /limine */
#define LIMINE_HDD_ISO_PATH   "/limine/limine-hdd.bin"    /* MBR boot + stage2 */

#define ROOTFS_EXT2  0
#define ROOTFS_FAT32 1

/* ------------------------------------------------------------------------- */
/* TUI colours (framebuffer RGB)                                             */
/* ------------------------------------------------------------------------- */

#define C_BG     0x00102040u   /* deep blue backdrop  */
#define C_FG     0x00FFFFFFu   /* white text          */
#define C_DIM    0x00A0B0C0u   /* muted text          */
#define C_TITLE  0x0040E0FFu   /* cyan title          */
#define C_SELFG  0x00102040u   /* selected text (dark)*/
#define C_SELBG  0x0040E0FFu   /* selected row (cyan) */
#define C_WARN   0x00FF6060u   /* red warning         */
#define C_OK     0x0060FF80u   /* green success       */

/* ------------------------------------------------------------------------- */
/* File-scope scratch (never on the kernel stack)                            */
/* ------------------------------------------------------------------------- */

static uint8_t  s_boot[512];      /* boot sector being assembled / read     */
static uint8_t  s_pt[70];         /* saved partition table bytes [440..510) */
static uint8_t *s_filebuf;        /* shared transfer buffer (kmalloc'd)     */

/* BFS work queue + per-directory entry list for the recursive tree copy.
 * Collected non-reentrantly: enumerate a whole directory into s_ents, then
 * act on it, so iso9660_read_file is never called from inside an
 * iso9660_complete callback. */
#define COPYQ_MAX  256
#define PATH_MAX_   192
static char s_copyq[COPYQ_MAX][PATH_MAX_];
static int  s_q_head, s_q_tail;

#define ENTS_MAX 160
static struct { char name[64]; int is_dir; } s_ents[ENTS_MAX];
static int s_nents;

/* Optional component trees, chosen in the wizard (default: install both). */
static int      s_inst_docs = 1;   /* copy /docs to the target rootfs */
static int      s_inst_src  = 1;   /* copy /src  to the target rootfs */

/* ------------------------------------------------------------------------- */
/* Geometry                                                                  */
/* ------------------------------------------------------------------------- */

/* The installer renders as a real ANSI/VT100 terminal application.  Output is
 * a byte stream of escape sequences written to the calling task's stdout via
 * kfd_stdout_write(): on a live text VT those bytes reach the framebuffer
 * console's ANSI parser (vt_putchar); inside an mxterm window they reach the
 * client's vt100 emulator over a pipe.  Pure-VGA-text with no terminal at all
 * (no VESA, not piped) degrades to a plain numbered-prompt flow via t_write. */
static int g_ansi;       /* 1 = ANSI terminal available, 0 = VGA-text fallback */
static uint32_t g_cols, g_rows;   /* usable area (status row excluded)       */

static void tui_geometry(void)
{
    g_ansi = kfd_stdout_is_pipe() || vesa_tty_is_ready();
    if (g_ansi) {
        kfd_term_size(&g_cols, &g_rows);
        if (g_cols == 0) g_cols = 80;
        if (g_rows < 2)  g_rows = 25;
    } else {
        g_cols = 80;
        g_rows = 49;   /* 80x50 VGA text, minus status row */
    }
}

/* ------------------------------------------------------------------------- */
/* ANSI output primitives                                                     */
/* ------------------------------------------------------------------------- */

/* Write to the invoking task's terminal; fall back to the kernel console when
 * it has no usable stdout (in-kernel rescue shell). */
static void out(const char *s)
{
    unsigned n = 0; while (s[n]) n++;
    if (kfd_stdout_write(s, n) < 0) t_write(s, n);
}
static void outn(const char *s, unsigned n)
{
    if (kfd_stdout_write(s, n) < 0) t_write(s, n);
}

/* uint -> decimal text, returns length written (no NUL). */
static int u2d(char *b, unsigned v)
{
    char t[10]; int n = 0;
    if (!v) { b[0] = '0'; return 1; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) b[i] = t[n - 1 - i];
    return n;
}

/* Emit SGR for an (fg,bg) pair drawn from the installer's RGB palette.  The
 * console / mxterm map these 16-colour codes back to pixels; bright codes
 * (9x/10x) carry the vivid title/warn/ok hues. */
static void ansi_color(uint32_t fg, uint32_t bg)
{
    if (!g_ansi) return;
    unsigned fc, bc;
    if      (fg == C_TITLE) fc = 96;        /* bright cyan */
    else if (fg == C_DIM)   fc = 37;        /* grey        */
    else if (fg == C_SELFG) fc = 30;        /* dark (on cyan selection) */
    else if (fg == C_WARN)  fc = 91;        /* bright red  */
    else if (fg == C_OK)    fc = 92;        /* bright green */
    else                    fc = 97;        /* C_FG bright white */
    if      (bg == C_SELBG) bc = 106;       /* bright cyan (== C_TITLE) */
    else if (bg == C_WARN)  bc = 101;
    else                    bc = 44;        /* C_BG deep blue / default */
    char b[24]; int o = 0;
    b[o++] = 0x1b; b[o++] = '['; b[o++] = '0'; b[o++] = ';';
    o += u2d(b + o, fc); b[o++] = ';'; o += u2d(b + o, bc); b[o++] = 'm';
    outn(b, (unsigned)o);
}
static void ansi_goto(uint32_t col, uint32_t row)
{
    if (!g_ansi) return;
    char b[24]; int o = 0;
    b[o++] = 0x1b; b[o++] = '[';
    o += u2d(b + o, row + 1); b[o++] = ';'; o += u2d(b + o, col + 1); b[o++] = 'H';
    outn(b, (unsigned)o);
}

/* Clear the whole screen to bg, cursor home. */
static void tui_clear(uint32_t bg)
{
    if (!g_ansi) return;
    ansi_color(C_FG, bg);
    out("\x1b[2J\x1b[H");
}

/* Paint a positioned, coloured string (no-op in VGA-text fallback).  Embedded
 * '\n' starts a new line re-anchored at the same start column (some menu
 * descriptions are multi-line) rather than wrapping to column 0. */
static void tui_at(uint32_t col, uint32_t row, const char *s,
                   uint32_t fg, uint32_t bg)
{
    if (!g_ansi) return;
    ansi_color(fg, bg);
    uint32_t line = 0;
    const char *p = s;
    for (;;) {
        ansi_goto(col, row + line);
        char buf[256]; int o = 0;
        while (*p && *p != '\n' && o < (int)sizeof(buf) - 1) buf[o++] = *p++;
        buf[o] = '\0';
        out(buf);
        if (*p != '\n') break;   /* hit end of string */
        p++; line++;             /* consume newline, continue one row down */
    }
}

/* Centre a string on 'row'. */
static void tui_center(uint32_t row, const char *s, uint32_t fg, uint32_t bg)
{
    size_t len = strlen(s);
    uint32_t col = (len < g_cols) ? (uint32_t)((g_cols - len) / 2) : 0;
    tui_at(col, row, s, fg, bg);
}

/* Common header (title bar + footer hint) for every wizard screen. */
static void tui_frame(const char *title, const char *hint)
{
    tui_clear(C_BG);
    char bar[256];
    uint32_t n = (g_cols < 255) ? g_cols : 255;
    for (uint32_t i = 0; i < n; i++) bar[i] = ' ';
    bar[n] = '\0';
    tui_at(0, 0, bar, C_SELFG, C_TITLE);
    tui_center(0, title, C_SELFG, C_TITLE);
    if (hint)
        tui_at(2, g_rows - 1, hint, C_DIM, C_BG);
}

/* ------------------------------------------------------------------------- */
/* Keyboard helpers                                                          */
/* ------------------------------------------------------------------------- */

/* One key.  Source is the mxterm stdin pipe when piped, else the raw keyboard;
 * arrow bytes (0x80-0x83), Esc (0x1B) and ASCII match either way.  EOF (pipe
 * writer gone) returns Esc so menus cancel rather than spin. */
static unsigned char getkey(void)
{
    int b = kfd_stdin_getbyte();
    return (b < 0) ? 0x1B : (unsigned char)b;
}

/* Read a line (VGA fallback prompts / "type yes" confirm) with echo. */
static void readline(char *buf, size_t max)
{
    size_t len = 0;
    while (1) {
        unsigned char c = getkey();
        if (c == '\n' || c == '\r') { out("\r\n"); break; }
        if (c == '\b' || c == 127) { if (len) { len--; out("\b \b"); } continue; }
        if (c < 0x20 || c > 0x7E) continue;
        if (len < max - 1) { buf[len++] = (char)c; { char e[2]={(char)c,0}; out(e); } }
    }
    buf[len] = '\0';
}

/* Masked password readline: echoes '*' per character with a '_' cursor at
 * (field_col, row) in ANSI mode; plain '*' echo in VGA-text fallback. */
static void readline_masked(char *buf, size_t max,
                            uint32_t field_col, uint32_t row)
{
    size_t len = 0;

    if (g_ansi)
        tui_at(field_col, row, "_", C_TITLE, C_BG);

    while (1) {
        unsigned char c = getkey();
        if (c == '\n' || c == '\r') {
            if (g_ansi)
                tui_at(field_col + (uint32_t)len, row, " ", C_FG, C_BG);
            else
                out("\r\n");
            break;
        }
        if (c == '\b' || c == 127) {
            if (len) {
                if (g_ansi) {
                    tui_at(field_col + (uint32_t)len, row, " ", C_FG, C_BG);
                    len--;
                    tui_at(field_col + (uint32_t)len, row, "_", C_TITLE, C_BG);
                } else {
                    len--; out("\b \b");
                }
            }
            continue;
        }
        if (c < 0x20 || c > 0x7E) continue;
        if (len < max - 1) {
            if (g_ansi)
                tui_at(field_col + (uint32_t)len, row, "*", C_FG, C_BG);
            else
                out("*");
            buf[len++] = (char)c;
            if (g_ansi)
                tui_at(field_col + (uint32_t)len, row, "_", C_TITLE, C_BG);
        }
    }
    buf[len] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Menu - arrow-selectable list (GUI) or numbered prompt (VGA)               */
/* Returns the chosen index, or -1 if cancelled (Esc).                       */
/* ------------------------------------------------------------------------- */

static int tui_menu(const char *title, const char *hint,
                    const char *const *items, int n,
                    const char *const *descs)
{
    if (!g_ansi) {
        /* VGA fallback: numbered list + readline. */
        t_writestring("\n=== ");
        t_writestring(title);
        t_writestring(" ===\n");
        for (int i = 0; i < n; i++) {
            t_putchar(' '); t_dec((uint32_t)(i + 1)); t_writestring(") ");
            t_writestring(items[i]);
            if (descs && descs[i]) { t_writestring("  - "); t_writestring(descs[i]); }
            t_putchar('\n');
        }
        char in[8];
        while (1) {
            t_writestring("Choice (number, or q to cancel): ");
            readline(in, sizeof(in));
            if (in[0] == 'q') return -1;
            int v = 0; const char *p = in;
            while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
            if (v >= 1 && v <= n) return v - 1;
            t_writestring("Invalid choice.\n");
        }
    }

    /* Vertical step per item: 1 label row + the tallest description + a blank
     * separator row, so multi-line descriptions never collide with the next
     * item (the bug that made the filesystem page look cramped). */
    int desc_lines = 0;
    if (descs)
        for (int i = 0; i < n; i++) {
            if (!descs[i]) continue;
            int ln = 1;
            for (const char *p = descs[i]; *p; p++) if (*p == '\n') ln++;
            if (ln > desc_lines) desc_lines = ln;
        }
    uint32_t step = desc_lines ? (uint32_t)(desc_lines + 2) : 2;

    int sel = 0;
    uint32_t top = 4;
    while (1) {
        tui_frame(title, hint ? hint : "Up/Down to move  Enter to select  Esc to cancel");
        for (int i = 0; i < n; i++) {
            uint32_t row = top + (uint32_t)i * step;
            char line[128];
            /* "  > label" with left padding; pad the rest with spaces so the
             * selected highlight spans a fixed width. */
            int o = 0;
            line[o++] = ' '; line[o++] = ' ';
            line[o++] = (i == sel) ? '>' : ' ';
            line[o++] = ' ';
            const char *t = items[i];
            while (*t && o < (int)sizeof(line) - 1) line[o++] = *t++;
            while (o < 40 && o < (int)sizeof(line) - 1) line[o++] = ' ';
            line[o] = '\0';
            if (i == sel)
                tui_at(4, row, line, C_SELFG, C_SELBG);
            else
                tui_at(4, row, line, C_FG, C_BG);
            if (descs && descs[i])
                tui_at(12, row + 1, descs[i], C_DIM, C_BG);   /* indent under the label */
        }
        unsigned char c = getkey();
        if (c == KEY_ARROW_UP)        sel = (sel == 0) ? n - 1 : sel - 1;
        else if (c == KEY_ARROW_DOWN) sel = (sel == n - 1) ? 0 : sel + 1;
        else if (c == '\n' || c == '\r') return sel;
        else if (c == 0x1B) return -1;   /* Esc */
    }
}

/* Modal confirmation.  GUI: a default-Cancel menu (arrow + Enter) - robust
 * against dropped keystrokes and the standard TUI idiom.  VGA: type "yes". */
static int tui_confirm(const char *title, const char *l1, const char *l2)
{
    if (g_ansi) {
        /* Paint the warning lines, then present the choice as a menu so the
         * (highlighted, reliable) arrow/Enter path drives it.  Default is
         * Cancel - the operator must move down to Install. */
        static const char *items[] = { "Cancel", "Yes - ERASE the disk and install" };
        /* tui_menu repaints the frame itself; stash the warnings above the
         * menu by painting them after the frame is drawn each loop is overkill,
         * so we fold the warning into the title hint line instead. */
        (void)l2;
        return tui_menu(title, l1, items, 2, NULL) == 1;
    }
    t_writestring("\n");
    t_writestring(l1); t_putchar('\n');
    if (l2) { t_writestring(l2); t_putchar('\n'); }
    t_writestring("Type 'yes' to proceed: ");
    char in[8];
    readline(in, sizeof(in));
    return strcmp(in, "yes") == 0;
}

/* ------------------------------------------------------------------------- */
/* Execution-phase log box (GUI) - a bordered, scrolling region so progress  */
/* stays inside the TUI instead of reverting to shell-style scrolling text.  */
/* ------------------------------------------------------------------------- */

#define LOG_LINES 96
#define LOG_W     118
static char     s_log[LOG_LINES][LOG_W];
static int      s_log_n;            /* total lines logged                    */
static uint32_t s_box_top, s_box_bot, s_box_left, s_box_w, s_box_vis;
static uint32_t s_status_row;

static void box_border(void)
{
    char top[256];
    uint32_t w = s_box_w;
    if (w > 250) w = 250;
    top[0] = '+';
    for (uint32_t i = 1; i < w - 1; i++) top[i] = '-';
    top[w - 1] = '+'; top[w] = '\0';
    tui_at(s_box_left, s_box_top, top, C_DIM, C_BG);
    tui_at(s_box_left, s_box_bot, top, C_DIM, C_BG);
    for (uint32_t r = s_box_top + 1; r < s_box_bot; r++) {
        tui_at(s_box_left, r, "|", C_DIM, C_BG);
        tui_at(s_box_left + w - 1, r, "|", C_DIM, C_BG);
    }
}

/* Repaint the visible tail of the log inside the box. */
static void box_repaint(void)
{
    if (!g_ansi) return;
    int start = (s_log_n > (int)s_box_vis) ? s_log_n - (int)s_box_vis : 0;
    char blank[LOG_W];
    for (uint32_t i = 0; i < s_box_w - 2 && i < LOG_W - 1; i++) blank[i] = ' ';
    blank[(s_box_w - 2 < LOG_W - 1) ? (s_box_w - 2) : (LOG_W - 1)] = '\0';
    for (uint32_t v = 0; v < s_box_vis; v++) {
        uint32_t row = s_box_top + 1 + v;
        tui_at(s_box_left + 1, row, blank, C_FG, C_BG);
        int li = start + (int)v;
        if (li < s_log_n)
            tui_at(s_box_left + 1, row, s_log[li % LOG_LINES], C_FG, C_BG);
    }
}

static void exec_screen(const char *title)
{
    s_log_n = 0;
    if (g_ansi) {
        tui_clear(C_BG);
        tui_frame(title, "Installing - please wait...");
        s_box_top   = 2;
        s_box_bot   = (g_rows > 4) ? g_rows - 3 : g_rows - 1;
        s_box_left  = 2;
        s_box_w     = (g_cols > 4) ? g_cols - 4 : g_cols;
        s_box_vis   = (s_box_bot > s_box_top + 1) ? (s_box_bot - s_box_top - 1) : 1;
        s_status_row = g_rows - 2;
        box_border();
    } else {
        t_writestring("\n=== ");
        t_writestring(title);
        t_writestring(" ===\n");
    }
}

/* Append one log line (a discrete progress step). */
static void tui_log(const char *s)
{
    /* Mirror every progress/error line into the in-RAM log tree at
     * /log/install.log.  In GUI mode the log box paints to the framebuffer
     * only, so without this a failed install leaves no inspectable record;
     * `cat /log/install.log` now shows the full step list up to the point it
     * stopped. */
    logfs_append_line("install.log", s);

    if (g_ansi) {
        char *dst = s_log[s_log_n % LOG_LINES];
        uint32_t i = 0;
        while (s[i] && i < LOG_W - 1) { dst[i] = s[i]; i++; }
        dst[i] = '\0';
        s_log_n++;
        box_repaint();
    } else {
        t_writestring(s);
        t_putchar('\n');
    }
}

/* Overwrite the transient status line (running counts during long copies). */
static void tui_status(const char *s)
{
    if (g_ansi) {
        char blank[256];
        uint32_t w = (g_cols < 255) ? g_cols : 255;
        for (uint32_t i = 0; i < w; i++) blank[i] = ' ';
        blank[w] = '\0';
        tui_at(0, s_status_row, blank, C_FG, C_BG);
        tui_at(2, s_status_row, s, C_TITLE, C_BG);
    }
    /* VGA: counts are noise without a status line; skip. */
}

/* Build "<prefix><uint>" into buf (no libc itoa available freestanding). */
static void str_u(char *buf, const char *prefix, uint32_t v)
{
    int o = 0;
    while (*prefix) buf[o++] = *prefix++;
    char num[12]; int ni = 0;
    if (v == 0) num[ni++] = '0';
    while (v) { num[ni++] = (char)('0' + v % 10); v /= 10; }
    while (ni) buf[o++] = num[--ni];
    buf[o] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Little-endian writers                                                     */
/* ------------------------------------------------------------------------- */

static void wr32(uint8_t *b, int off, uint32_t v)
{
    b[off] = (uint8_t)v; b[off+1] = (uint8_t)(v>>8);
    b[off+2] = (uint8_t)(v>>16); b[off+3] = (uint8_t)(v>>24);
}
static void wr64(uint8_t *b, int off, uint64_t v)
{
    for (int i = 0; i < 8; i++) b[off+i] = (uint8_t)(v >> (8*i));
}

/* ------------------------------------------------------------------------- */
/* File / tree copy (ISO -> rootfs)                                          */
/* ------------------------------------------------------------------------- */

static int g_root_fs;    /* ROOTFS_EXT2 / ROOTFS_FAT32 (selected backend)    */
static uint8_t g_cd;     /* CD-ROM IDE drive index                          */

static int rfs_mkdir(const char *p)
{ return (g_root_fs == ROOTFS_EXT2) ? ext2_mkdir(p) : fat32_mkdir(p); }
static int rfs_write(const char *p, const void *b, uint32_t n)
{ return (g_root_fs == ROOTFS_EXT2) ? ext2_write_file(p, b, n)
                                    : fat32_write_file(p, b, n); }

static int copy_file(const char *src, const char *dst)
{
    uint32_t sz = 0;
    if (iso9660_read_file(g_cd, src, s_filebuf, INST_MAX_FILE_SIZE, &sz) != 0)
        return -1;
    if (rfs_write(dst, s_filebuf, sz) != 0)
        return -2;
    return 0;
}

/* Copy one named file and log a single step line with its size. */
static void copy_one(const char *src, const char *dst)
{
    char line[LOG_W];
    int o = 0;
    line[o++] = ' '; line[o++] = ' ';
    const char *p = dst;
    while (*p && o < LOG_W - 24) line[o++] = *p++;
    const char *tail = " ... ";
    while (*tail) line[o++] = *tail++;
    uint32_t sz = 0;
    if (iso9660_read_file(g_cd, src, s_filebuf, INST_MAX_FILE_SIZE, &sz) != 0) {
        const char *e = "(skip)"; while (*e) line[o++] = *e++; line[o] = '\0';
        tui_log(line); return;
    }
    if (rfs_write(dst, s_filebuf, sz) != 0) {
        const char *e = "write error"; while (*e) line[o++] = *e++; line[o] = '\0';
        tui_log(line); return;
    }
    /* append "<sz> B" */
    char num[12]; int ni = 0; uint32_t v = sz;
    if (v == 0) num[ni++] = '0';
    while (v) { num[ni++] = (char)('0' + v % 10); v /= 10; }
    while (ni) line[o++] = num[--ni];
    line[o++] = ' '; line[o++] = 'B'; line[o] = '\0';
    tui_log(line);
}

/* iso9660_complete callback - record entries (no I/O here). */
static void enum_cb(const char *name, int is_dir, void *ctx)
{
    (void)ctx;
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
        return;
    if (s_nents >= ENTS_MAX) return;
    size_t i = 0;
    while (name[i] && i < sizeof(s_ents[0].name) - 1) { s_ents[s_nents].name[i] = name[i]; i++; }
    s_ents[s_nents].name[i] = '\0';
    s_ents[s_nents].is_dir = is_dir;
    s_nents++;
}

static void q_reset(void) { s_q_head = s_q_tail = 0; }
static int  q_empty(void) { return s_q_head == s_q_tail; }
static void q_push(const char *p)
{
    int nxt = (s_q_tail + 1) % COPYQ_MAX;
    if (nxt == s_q_head) return;   /* full - drop (bounded) */
    size_t i = 0;
    while (p[i] && i < PATH_MAX_ - 1) { s_copyq[s_q_tail][i] = p[i]; i++; }
    s_copyq[s_q_tail][i] = '\0';
    s_q_tail = nxt;
}
static const char *q_pop(void)
{
    const char *p = s_copyq[s_q_head];
    s_q_head = (s_q_head + 1) % COPYQ_MAX;
    return p;
}


/* ------------------------------------------------------------------------- */
/* limine MBR install (mirrors host/limine.c::bios_install, MBR path, v12.x) */
/* ------------------------------------------------------------------------- */

static int write_sectors_batched(uint8_t drive, uint32_t lba,
                                 const uint8_t *buf, uint32_t sectors)
{
    uint32_t done = 0;
    while (done < sectors) {
        uint32_t batch = sectors - done;
        if (batch > 127u) batch = 127u;
        if (ide_write_sectors(drive, lba + done, (uint8_t)batch,
                              (void *)(buf + done * 512u)) != 0)
            return -1;
        done += batch;
    }
    return 0;
}

static int limine_install_mbr(uint8_t drive, uint8_t *sys, uint32_t sys_sz)
{
    if (sys_sz <= 512u) return -1;
    uint32_t stage2_sz    = sys_sz - 512u;
    uint32_t stage2_sects = (stage2_sz + 511u) / 512u;

    /* Stage 2 lives in the post-MBR gap; it must end before the first
     * partition (LBA 2048). */
    if (1u + stage2_sects > INST_PART_START_LBA) return -2;

    /* Read the current MBR to preserve its partition table + timestamp. */
    if (ide_read_sectors(drive, 0u, 1u, s_boot) != 0) return -3;
    uint8_t save_ts[6];
    memcpy(save_ts, s_boot + 218, 6);
    memcpy(s_pt, s_boot + 440, 70);

    /* Build the new boot sector from limine's first 512 bytes, then restore
     * the disk's partition table + timestamp on top (exactly as bios-install
     * does), and patch the stage-2 location. */
    memcpy(s_boot, sys, 512);
    memcpy(s_boot + 218, save_ts, 6);
    memcpy(s_boot + 440, s_pt, 70);
    wr64(s_boot, LIMINE_STAGE2_LOC_OFF, 512ull);   /* stage2 at byte 512      */

    /* Write stage 2 into the gap (sector 1..), zero-padding the last sector. */
    uint32_t padded = stage2_sects * 512u;
    if (padded > stage2_sz)
        memset(sys + 512u + stage2_sz, 0, padded - stage2_sz);
    if (write_sectors_batched(drive, 1u, sys + 512u, stage2_sects) != 0) return -4;

    /* Write the patched boot sector last. */
    if (ide_write_sectors(drive, 0u, 1u, s_boot) != 0) return -5;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Disk / source discovery                                                   */
/* ------------------------------------------------------------------------- */

static int find_cdrom(void)
{
    for (int i = 0; i < IDE_MAX_DRIVES; i++) {
        const ide_drive_t *d = ide_get_drive((uint8_t)i);
        if (d && d->present && d->type == IDE_TYPE_ATAPI &&
            iso9660_probe((uint8_t)i) == 0)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Whole-disk partition (one rootfs partition spanning the drive)            */
/* ------------------------------------------------------------------------- */

static int partition_whole_disk(uint8_t drive, uint32_t disk_sectors,
                                int fs,
                                uint32_t *boot_lba, uint32_t *boot_count,
                                uint32_t *data_lba, uint32_t *data_count)
{
    uint32_t b_start = INST_PART_START_LBA;
    uint32_t b_sz    = INST_BOOT_SECTORS;
    uint32_t d_start = b_start + b_sz;
    if (disk_sectors <= d_start) {
        tui_log("  ERROR: disk too small for dual-partition layout (need > 34 MiB bootfs + rootfs headroom).");
        return -1;
    }
    uint32_t d_sz = disk_sectors - d_start;

    memset(s_boot, 0, sizeof(s_boot));
    uint8_t *pe = s_boot + MBR_PART_TABLE_OFF;

    /* Entry 0: FAT32 boot partition (kernel + limine stage 3) */
    pe[0] = 0x80;                                              /* bootable */
    pe[1] = 0xFE; pe[2] = 0xFF; pe[3] = 0xFF;
    pe[4] = PART_MBR_FAT32_LBA;
    pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;
    wr32(pe, 8, b_start);
    wr32(pe, 12, b_sz);

    /* Entry 1: data partition (apps / docs / src) */
    pe += 16;
    pe[0] = 0x00;
    pe[1] = 0xFE; pe[2] = 0xFF; pe[3] = 0xFF;
    pe[4] = (fs == ROOTFS_EXT2) ? 0x83u : PART_MBR_FAT32_LBA;
    pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;
    wr32(pe, 8, d_start);
    wr32(pe, 12, d_sz);

    s_boot[MBR_SIG_OFF] = 0x55; s_boot[MBR_SIG_OFF + 1] = 0xAA;

    if (ide_write_sectors(drive, 0u, 1u, s_boot) != 0) return -2;
    *boot_lba = b_start; *boot_count = b_sz;
    *data_lba = d_start; *data_count = d_sz;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Execution phase                                                           */
/* ------------------------------------------------------------------------- */

/* Base config: the plain text entry plus the head of a GUI desktop entry whose
 * cmdline is completed at write time with "[autologin=<user> ]autoboot=gui", so
 * selecting it boots straight to the desktop as the configured autologin user. */
static const char limine_head[] =
    "# Generated by the Makar installer.\n"
    "timeout: 3\n"
    /* Request 720p; Limine falls back to the closest mode the firmware offers
     * (e.g. 1024x768 on Hyper-V Gen1, which has no 16:9 VBE modes). */
    "resolution: 1280x720x32\n"
    "\n"
    "/Makar OS\n"
    "    protocol: multiboot2\n"
    "    path: boot():/boot/makar.kernel\n"
    "\n"
    "/Makar OS (GUI desktop)\n"
    "    protocol: multiboot2\n"
    "    path: boot():/boot/makar.kernel\n"
    "    cmdline: ";

/* Build the full limine.conf into buf; returns its length.  autologin_user may
 * be empty (then the GUI entry carries just "autoboot=gui" and falls back to
 * /etc/autologin or the login prompt). */
static uint32_t build_limine_conf(char *buf, uint32_t cap, const char *autologin_user)
{
    uint32_t o = 0;
    uint32_t hl = (uint32_t)strlen(limine_head);
    if (hl >= cap) return 0;
    memcpy(buf, limine_head, hl); o = hl;
    if (autologin_user && autologin_user[0]) {
        const char *al = "autologin=";
        for (int i = 0; al[i] && o < cap - 1; i++) buf[o++] = al[i];
        for (int i = 0; autologin_user[i] && o < cap - 1; i++) buf[o++] = autologin_user[i];
        if (o < cap - 1) buf[o++] = ' ';
    }
    const char *tail =
        "autoboot=gui\n"
        "\n"
        "/Makar OS (rescue shell)\n"
        "    protocol: multiboot2\n"
        "    path: boot():/boot/makar.kernel\n"
        "    cmdline: shell=rescue\n"
        "\n"
        "/Makar OS (verbose boot)\n"
        "    protocol: multiboot2\n"
        "    path: boot():/boot/makar.kernel\n"
        "    cmdline: verbose\n"
        "\n"
        "/Makar OS (serial console)\n"
        "    protocol: multiboot2\n"
        "    path: boot():/boot/makar.kernel\n"
        "    cmdline: console=ttyS0\n";
    for (int i = 0; tail[i] && o < cap - 1; i++) buf[o++] = tail[i];
    buf[o] = '\0';
    return o;
}



/* ------------------------------------------------------------------------- */
/* Drive picker                                                              */
/* ------------------------------------------------------------------------- */

/* Builds the menu label arrays from the live ATA drive list.  Returns the
 * count, fills idx_map[] with the IDE indices. */
#define MAX_PICK 4
static char        s_pick_lbl[MAX_PICK][64];
static const char *s_pick_ptr[MAX_PICK];
static int         s_pick_idx[MAX_PICK];

static int build_drive_list(void)
{
    int n = 0;
    for (int i = 0; i < IDE_MAX_DRIVES && n < MAX_PICK; i++) {
        const ide_drive_t *d = ide_get_drive((uint8_t)i);
        if (!d || !d->present || d->type != IDE_TYPE_ATA) continue;
        /* "hdX  (NNN MiB)  model" */
        char *L = s_pick_lbl[n];
        int o = 0;
        L[o++] = 'h'; L[o++] = 'd'; L[o++] = (char)('a' + n); L[o++] = ' '; L[o++] = ' ';
        L[o++] = '(';
        uint32_t mib = d->size / 2048u;
        char num[12]; int ni = 0;
        if (mib == 0) num[ni++] = '0';
        while (mib) { num[ni++] = (char)('0' + mib % 10); mib /= 10; }
        while (ni) L[o++] = num[--ni];
        L[o++] = ' '; L[o++] = 'M'; L[o++] = 'i'; L[o++] = 'B'; L[o++] = ')';
        L[o++] = ' '; L[o++] = ' ';
        const char *m = d->model;
        while (*m && o < 60) L[o++] = *m++;
        L[o] = '\0';
        s_pick_ptr[n] = L;
        s_pick_idx[n] = i;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* Shared headless execution engine (kernel/installer.h)                     */
/*                                                                           */
/* Drives the same disk work the TUI wizard performs, but parameterised and  */
/* *stepped* so a GUI front-end (mxinstall.elf) can render a live progress   */
/* bar and yield between files instead of freezing behind one blocking call. */
/* ------------------------------------------------------------------------- */

/* Build a shadow line "user:$mh$<salt16>$<hash16>:::::::\n" into out (>=128).
 * Returns its length.  Salt seeded from the timer + RTC like the TUI path. */
static uint32_t make_shadow_line(const char *user, const char *pw, char *out)
{
    char salt[17], hash[17];
    uint32_t ticks = timer_get_ticks();
    uint32_t rtcsec = 0; rtc_unix_time(&rtcsec);
    uint8_t seed[8];
    seed[0]=(uint8_t)ticks;        seed[1]=(uint8_t)(ticks>>8);
    seed[2]=(uint8_t)(ticks>>16);  seed[3]=(uint8_t)(ticks>>24);
    seed[4]=(uint8_t)rtcsec;       seed[5]=(uint8_t)(rtcsec>>8);
    seed[6]=(uint8_t)(rtcsec>>16); seed[7]=(uint8_t)(rtcsec>>24);
    microhash_hex16(seed, 8, salt);
    {
        size_t plen = strlen(pw); if (plen > 256) plen = 256;
        uint8_t combined[16 + 256];
        for (int i = 0; i < 16; i++) combined[i] = (uint8_t)salt[i];
        for (size_t i = 0; i < plen; i++) combined[16+i] = (uint8_t)pw[i];
        microhash_hex16(combined, 16 + plen, hash);
    }
    uint32_t pos = 0;
    for (const char *u = user; *u && pos < 64; u++) out[pos++] = *u;
    out[pos++]=':'; out[pos++]='$'; out[pos++]='m'; out[pos++]='h'; out[pos++]='$';
    for (int i=0;i<16;i++) out[pos++]=salt[i];
    out[pos++]='$';
    for (int i=0;i<16;i++) out[pos++]=hash[i];
    const char *tail = ":::::::\n"; for (int i=0;tail[i];i++) out[pos++]=tail[i];
    out[pos]='\0';
    return pos;
}

/* Default per-account rc files (mirrors the TUI installer's). */
static const char s_makrc[]  = "# ~/.makshrc -- sourced by sh.elf on login\nPATH=/apps:/bin\n";
static const char s_vixrc[]  = "\" ~/.vixrc -- read by vix on startup\nset linenumbers\n";
static const char s_sbrc[]   =
    "# ~/.sbrc -- statusbar layout: <section> <widgets...>\n"
    "left command\ncenter tabs\nright cpu mem rootfs time\n";

/* Write the home dir + .makshrc/.vixrc/.sbrc for one account. */
static void write_home(const char *home)
{
    rfs_mkdir(home);
    char path[96];
    const char *files[3] = { "/.makshrc", "/.vixrc", "/.sbrc" };
    const char *body[3]  = { s_makrc, s_vixrc, s_sbrc };
    for (int f = 0; f < 3; f++) {
        int o = 0; const char *h = home;
        while (*h && o < (int)sizeof(path)-12) path[o++] = *h++;
        const char *s = files[f]; while (*s) path[o++] = *s++; path[o] = '\0';
        rfs_write(path, body[f], (uint32_t)strlen(body[f]));
    }
}

/* Write /etc/{hostname,shadow,autologin} + home dirs from the params.  The data
 * partition must already be mounted (g_root_fs set). */
static void install_write_meta(const install_params_t *p)
{
    rfs_mkdir("/etc");

    const char *host = (p->hostname[0]) ? p->hostname : "makar";
    { char hf[66]; int n=0; for (const char *h=host; *h && n<64; h++) hf[n++]=*h;
      hf[n++]='\n'; hf[n]='\0'; rfs_write("/etc/hostname", hf, (uint32_t)n); }

    /* /etc/shadow: root (if a password was set) + optional user. */
    {
        char shadow[512]; uint32_t sp = 0;
        if (p->root_pw[0]) {
            char ln[160]; uint32_t l = make_shadow_line("root", p->root_pw, ln);
            for (uint32_t i=0;i<l && sp<sizeof(shadow);i++) shadow[sp++]=ln[i];
        }
        if (p->user_name[0] && p->user_pw[0]) {
            char ln[160]; uint32_t l = make_shadow_line(p->user_name, p->user_pw, ln);
            for (uint32_t i=0;i<l && sp<sizeof(shadow);i++) shadow[sp++]=ln[i];
        }
        if (sp) rfs_write("/etc/shadow", shadow, sp);
    }

    if (p->autologin[0]) {
        char al[66]; int n=0; for (const char *a=p->autologin; *a && n<64; a++) al[n++]=*a;
        al[n++]='\n'; al[n]='\0'; rfs_write("/etc/autologin", al, (uint32_t)n);
    }

    /* Home directories. */
    rfs_mkdir("/root"); rfs_mkdir("/home");
    if (p->root_pw[0]) write_home("/root");
    if (p->user_name[0] && p->user_pw[0]) {
        char home[80]; int o=0; const char *pre="/home/";
        while (*pre) home[o++]=*pre++;
        for (const char *u=p->user_name; *u && o<(int)sizeof(home)-1; u++) home[o++]=*u;
        home[o]='\0';
        write_home(home);
    }
}

/* Copy-iterator state (BFS across the selected trees, one file per step). */
static const char *s_gui_trees[4];
static int         s_gui_ntrees, s_gui_tree_i, s_gui_ent_i;
static uint32_t    s_gui_files, s_gui_total;
static char        s_gui_dir[PATH_MAX_];

/* Count files across all selected trees up-front, so the copy phase can report
 * an accurate percentage.  A pure directory walk (no file reads), so it's cheap
 * relative to the copy itself; uses the shared BFS scratch (s_copyq/s_ents). */
static uint32_t install_count_files(void)
{
    uint32_t total = 0;
    for (int t = 0; t < s_gui_ntrees; t++) {
        q_reset();
        q_push(s_gui_trees[t]);
        while (!q_empty()) {
            char dir[PATH_MAX_];
            strncpy(dir, q_pop(), PATH_MAX_ - 1);
            dir[PATH_MAX_ - 1] = '\0';
            s_nents = 0;
            iso9660_complete(g_cd, dir, "", enum_cb, NULL);
            for (int i = 0; i < s_nents; i++) {
                if (!s_ents[i].is_dir) { total++; continue; }
                char child[PATH_MAX_]; int co = 0;
                for (const char *d = dir; *d && co < PATH_MAX_ - 1; d++) child[co++] = *d;
                if (!(co == 1 && child[0] == '/') && co < PATH_MAX_ - 1) child[co++] = '/';
                for (const char *nm = s_ents[i].name; *nm && co < PATH_MAX_ - 1; nm++) child[co++] = *nm;
                child[co] = '\0';
                q_push(child);
            }
        }
    }
    return total;
}

/* Run the install engine preemptibly: syscalls normally execute with
 * interrupts masked (non-preemptible), which froze the whole machine -- mouse
 * included -- for the duration of a disk-bound install.  Enabling interrupts
 * here lets the timer preempt on its fixed schedule, so the compositor and
 * other tasks keep running at full frame rate while files copy in the
 * background.  The shared state this path touches is made safe for that: the
 * heap is irq-guarded and the IDE controller is behind a lock.  (The FS
 * scratch buffers are only safe because the engine is the sole disk-FS user
 * during an install; concurrent FS from another task is the remaining gap,
 * tracked for the full preemptive-kernel pass.) */
static inline void inst_preempt_on(void) { __asm__ volatile("sti"); }

int install_exec_begin(const install_params_t *p)
{
    inst_preempt_on();
    if (!p) return -1;
    int cd = find_cdrom(); if (cd < 0) return -2;
    g_cd = (uint8_t)cd;
    if (!s_filebuf) { s_filebuf = (uint8_t *)kmalloc(INST_MAX_FILE_SIZE); if (!s_filebuf) return -3; }

    const ide_drive_t *hdd = ide_get_drive(p->drive);
    if (!hdd) return -4;
    int fs = (p->fs == INSTALL_FS_FAT32) ? ROOTFS_FAT32 : ROOTFS_EXT2;

    uint32_t b_lba=0,b_cnt=0,d_lba=0,d_cnt=0;
    if (partition_whole_disk(p->drive, hdd->size, fs, &b_lba,&b_cnt,&d_lba,&d_cnt) != 0) return -5;
    if (fat32_mkfs(p->drive, b_lba, b_cnt) != 0) return -6;
    int mk = (fs == ROOTFS_EXT2) ? ext2_mkfs(p->drive, d_lba, d_cnt)
                                  : fat32_mkfs(p->drive, d_lba, d_cnt);
    if (mk != 0) return -7;

    /* limine MBR. */
    uint32_t sys_sz = 0;
    if (iso9660_read_file(g_cd, LIMINE_HDD_ISO_PATH, s_filebuf, INST_MAX_FILE_SIZE, &sys_sz) != 0
        || sys_sz <= 512) return -8;
    if (limine_install_mbr(p->drive, s_filebuf, sys_sz) != 0) return -9;

    /* Boot partition files. */
    if (fat32_mounted()) fat32_unmount();
    if (fat32_mount(p->drive, b_lba) != 0) return -10;
    g_root_fs = ROOTFS_FAT32;
    fat32_mkdir("/boot"); fat32_mkdir("/limine");
    copy_one("/boot/makar.kernel", "/boot/makar.kernel");
    copy_one(LIMINE_SYS_ISO_PATH, "/limine/limine-bios.sys");
    { static char lbuf[1024]; uint32_t ln = build_limine_conf(lbuf, sizeof lbuf, p->autologin);
      rfs_write("/limine/limine.conf", lbuf, ln); }
    fat32_unmount();

    /* Data partition: mount + metadata, leave mounted for the copy steps. */
    int mnt = (fs == ROOTFS_EXT2) ? ext2_mount(p->drive, d_lba) : fat32_mount(p->drive, d_lba);
    if (mnt != 0) return -11;
    g_root_fs = fs;
    install_write_meta(p);

    /* Build the tree list, count files for the progress %, then seed the
     * copy iterator. */
    s_gui_ntrees = 0;
    s_gui_trees[s_gui_ntrees++] = "/apps";
    if (p->install_docs) s_gui_trees[s_gui_ntrees++] = "/docs";
    if (p->install_src)  s_gui_trees[s_gui_ntrees++] = "/src";
    s_gui_trees[s_gui_ntrees++] = "/usr";
    s_gui_total = install_count_files();
    s_gui_tree_i = 0; s_gui_files = 0;
    s_nents = 0; s_gui_ent_i = 0;
    q_reset();
    rfs_mkdir(s_gui_trees[0]); q_push(s_gui_trees[0]);
    { char b[48]; str_u(b, "INSTALL>exec begin ok, files=", s_gui_total);
      Serial_WriteString(b); Serial_WriteString("\n"); }
    return 0;
}

int install_exec_step(install_progress_t *prog)
{
    inst_preempt_on();
    if (prog) { prog->done = 0; prog->files = s_gui_files; prog->total = s_gui_total; prog->current[0] = '\0'; }
    for (;;) {
        if (s_gui_ent_i >= s_nents) {
            /* Current directory drained: take the next dir, else next tree. */
            if (!q_empty()) {
                strncpy(s_gui_dir, q_pop(), PATH_MAX_ - 1);
                s_gui_dir[PATH_MAX_ - 1] = '\0';
                s_nents = 0;
                iso9660_complete(g_cd, s_gui_dir, "", enum_cb, NULL);
                s_gui_ent_i = 0;
                continue;
            }
            s_gui_tree_i++;
            if (s_gui_tree_i >= s_gui_ntrees) {
                if (prog) { prog->done = 1; prog->files = s_gui_files; prog->total = s_gui_total; }
                Serial_WriteString("INSTALL>copy done\n");
                return 0;   /* all trees copied */
            }
            q_reset();
            rfs_mkdir(s_gui_trees[s_gui_tree_i]);
            q_push(s_gui_trees[s_gui_tree_i]);
            s_nents = 0; s_gui_ent_i = 0;
            continue;
        }

        int idx = s_gui_ent_i++;
        /* Build "<dir>/<name>". */
        char child[PATH_MAX_]; int co = 0;
        for (const char *d = s_gui_dir; *d && co < PATH_MAX_ - 1; d++) child[co++] = *d;
        if (!(co == 1 && child[0] == '/') && co < PATH_MAX_ - 1) child[co++] = '/';
        for (const char *nm = s_ents[idx].name; *nm && co < PATH_MAX_ - 1; nm++) child[co++] = *nm;
        child[co] = '\0';

        if (s_ents[idx].is_dir) {
            rfs_mkdir(child);
            q_push(child);
            continue;   /* directories are quick: keep going within this step */
        }
        copy_file(child, child);
        s_gui_files++;
        if (prog) {
            prog->files = s_gui_files;
            prog->total = s_gui_total;
            int o = 0; for (const char *nm = s_ents[idx].name; *nm && o < (int)sizeof(prog->current)-1; nm++)
                prog->current[o++] = *nm;
            prog->current[o] = '\0';
        }
        /* Per-file serial breadcrumb with running count + percentage. */
        {
            unsigned pct = s_gui_total ? (s_gui_files * 100u) / s_gui_total : 100u;
            char b[160]; int o = 0;
            const char *t = "INSTALL>copy ";       while (*t) b[o++] = *t++;
            { char n[12]; int ni=0; uint32_t v=s_gui_files; if(!v)n[ni++]='0'; while(v){n[ni++]=(char)('0'+v%10);v/=10;} while(ni)b[o++]=n[--ni]; }
            b[o++]='/';
            { char n[12]; int ni=0; uint32_t v=s_gui_total; if(!v)n[ni++]='0'; while(v){n[ni++]=(char)('0'+v%10);v/=10;} while(ni)b[o++]=n[--ni]; }
            b[o++]=' '; b[o++]='(';
            { char n[12]; int ni=0; uint32_t v=pct; if(!v)n[ni++]='0'; while(v){n[ni++]=(char)('0'+v%10);v/=10;} while(ni)b[o++]=n[--ni]; }
            b[o++]='%'; b[o++]=')'; b[o++]=' ';
            for (const char *nm = s_ents[idx].name; *nm && o < (int)sizeof(b)-2; nm++) b[o++]=*nm;
            b[o++]='\n'; b[o]='\0';
            Serial_WriteString(b);
        }
        return 1;   /* one file copied; more work remains */
    }
}

int install_exec_drives(install_drive_t *out)
{
    if (!out) return 0;
    int n = 0;
    for (int i = 0; i < IDE_MAX_DRIVES && n < INSTALL_MAX_DRIVES; i++) {
        const ide_drive_t *d = ide_get_drive((uint8_t)i);
        if (!d || !d->present || d->type != IDE_TYPE_ATA) continue;
        out[n].index    = (unsigned char)i;
        out[n].size_mib = d->size / 2048u;
        int o = 0;
        for (const char *m = d->model; *m && o < (int)sizeof(out[n].model)-1; m++)
            out[n].model[o++] = *m;
        out[n].model[o] = '\0';
        n++;
    }
    return n;
}

int install_exec_finish(const install_params_t *p)
{
    inst_preempt_on();
    int fs = (p && p->fs == INSTALL_FS_FAT32) ? ROOTFS_FAT32 : ROOTFS_EXT2;
    if (rfs_mkdir("/bin") != 0)
        Serial_WriteString("[install] WARNING: mkdir /bin failed\n");
    if (fs == ROOTFS_EXT2) ext2_unmount(); else fat32_unmount();
    if (s_filebuf) { kfree(s_filebuf); s_filebuf = NULL; }
    Serial_WriteString("INSTALL>exec finish ok\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* installer_run                                                             */
/* ------------------------------------------------------------------------- */

static void installer_run_inner(void);

/* Public entry: run the wizard with the calling task renamed "installer" so
 * the userspace status bar's `command` widget reflects what's on screen.  The
 * name is restored on every exit path (the success path reboots, so its
 * restore is moot).  name is just a const char* pointer, so swapping it to a
 * string literal and back is safe. */
void installer_run(void)
{
    task_t     *me    = task_current();
    const char *saved = me ? me->name : (const char *)0;
    if (me) me->name = "installer";
    installer_run_inner();
    if (me) me->name = saved;
}

static void installer_run_inner(void)
{
    tui_geometry();
    Serial_WriteString("INSTALL>welcome\n");
    /* Pin /log/install.log as soon as the installer is entered so the
     * post-mortem `cat /log/install.log` always has something to show --
     * even if the user immediately Esc's out of the welcome screen, the
     * file still exists with a startup banner. */
    tui_log("Installer started.");

    /* Welcome. */
    if (g_ansi) {
        tui_frame("Makar OS Installer", "Press Enter to begin, Esc to cancel");
        tui_center(6,  "Install Makar to a hard disk.", C_FG, C_BG);
        tui_center(8,  "This will ERASE the drive you choose.", C_WARN, C_BG);
        tui_center(10, "34 MiB FAT32 boot + data partition (ext2 or FAT32) + apps + docs + src", C_DIM, C_BG);
        unsigned char c = getkey();
        if (c == 0x1B) return;
    } else {
        t_writestring("\n=== Makar OS Installer ===\n");
    }

    int cd = find_cdrom();
    if (cd < 0) {
        if (g_ansi) { tui_frame("Installer", "Press a key"); tui_center(6, "No ISO9660 CD-ROM source found.", C_WARN, C_BG); getkey(); }
        else t_writestring("Error: no ISO9660 CD-ROM source found.\n");
        return;
    }
    g_cd = (uint8_t)cd;

    int n = build_drive_list();
    if (n == 0) {
        if (g_ansi) { tui_frame("Installer", "Press a key"); tui_center(6, "No ATA target drives found.", C_WARN, C_BG); getkey(); }
        else t_writestring("Error: no ATA target drives found.\n");
        return;
    }

    Serial_WriteString("INSTALL>drive\n");
    int pick = tui_menu("Select target drive", NULL,
                        s_pick_ptr, n, NULL);
    if (pick < 0) return;
    int drive = s_pick_idx[pick];
    const ide_drive_t *hdd = ide_get_drive((uint8_t)drive);
    if (!hdd) return;

    /* Filesystem choice for the data partition. */
    static const char *fs_items[] = { "ext2  (Unix-style, recommended)", "FAT32 (EFI-style)" };
    static const char *fs_descs[] = {
        "/apps, /docs and /src land on an ext2 data partition.\n"
        "A separate 34 MiB FAT32 partition holds the kernel + Limine.",
        "/apps, /docs and /src land on a FAT32 data partition.\n"
        "A separate 34 MiB FAT32 partition holds the kernel + Limine." };
    Serial_WriteString("INSTALL>fs\n");
    int fs_pick = tui_menu("Root filesystem", NULL, fs_items, 2, fs_descs);
    if (fs_pick < 0) return;
    int fs = (fs_pick == 0) ? ROOTFS_EXT2 : ROOTFS_FAT32;

    /* Partition mode. */
    static const char *pm_items[] = { "Use entire disk (recommended)",
                                       "Advanced: edit layout in cfdisk" };
    static const char *pm_descs[] = {
        "Auto: 34 MiB FAT32 boot partition + data partition spanning rest.",
        "Quit here and run 'cfdisk' yourself, then re-run install." };
    Serial_WriteString("INSTALL>partition\n");
    int pm = tui_menu("Partitioning", NULL, pm_items, 2, pm_descs);
    if (pm < 0) return;
    if (pm == 1) {
        /* Advanced: defer to cfdisk.  We don't drive it here; instruct the
         * user and bail (re-running install with a partitioned disk picks up
         * the existing layout in a future revision). */
        if (g_ansi) {
            tui_frame("Advanced partitioning", "Press a key to exit the installer");
            tui_center(6, "Run 'cfdisk' to lay out partitions,", C_FG, C_BG);
            tui_center(7, "then re-run 'install' and choose 'Use entire disk'", C_DIM, C_BG);
            tui_center(8, "is not yet wired for custom partitions.", C_DIM, C_BG);
            getkey();
        } else {
            t_writestring("Run 'cfdisk' to partition, then re-run install.\n");
        }
        return;
    }

    /* Confirm destruction.  Emit a serial breadcrumb first so the test driver
     * can sync on it (the GUI confirm is a framebuffer-only menu with
     * no serial mirror, so without this marker the test had to blind-pause). */
    Serial_WriteString("INSTALL>confirm\n");
    char w1[80];
    {
        int o = 0; const char *p = "WARNING: ALL DATA on ";
        while (*p) w1[o++] = *p++;
        w1[o++] = 'h'; w1[o++] = 'd'; w1[o++] = (char)('a' + pick);
        p = " will be ERASED."; while (*p) w1[o++] = *p++;
        w1[o] = '\0';
    }
    if (!tui_confirm("Confirm install", w1, NULL)) {
        if (!g_ansi) t_writestring("Installation cancelled.\n");
        return;
    }

    /* ---- Optional components: docs + source trees (default: install both) ---- */
    Serial_WriteString("INSTALL>components\n");
    {
        unsigned char a1 = 0, a2 = 0;
        if (g_ansi) {
            tui_frame("Components", "Choose which optional trees to install.");
            uint32_t mid  = g_rows / 2;
            uint32_t lcol = (g_cols / 2) > 20 ? (g_cols / 2) - 20 : 2;
            tui_at(lcol, mid - 1, "Install documentation (/docs)? [Y/n]", C_FG, C_BG);
            a1 = getkey();
            tui_at(lcol, mid + 1, "Install source code (/src)? [Y/n]", C_FG, C_BG);
            a2 = getkey();
        } else {
            char buf[8];
            t_writestring("Install documentation (/docs)? [Y/n] ");
            readline(buf, sizeof(buf)); a1 = (unsigned char)buf[0];
            t_writestring("Install source code (/src)? [Y/n] ");
            readline(buf, sizeof(buf)); a2 = (unsigned char)buf[0];
        }
        s_inst_docs = (a1 == 'n' || a1 == 'N') ? 0 : 1;
        s_inst_src  = (a2 == 'n' || a2 == 'N') ? 0 : 1;
        Serial_WriteString(s_inst_docs ? "[install] docs: yes\n" : "[install] docs: no\n");
        Serial_WriteString(s_inst_src  ? "[install] src: yes\n"  : "[install] src: no\n");
    }

    /* ------------------------------------------------------------------ */
    /* Wizard: collect hostname + account data (no disk I/O)              */
    /* All inputs are gathered first; a single mount writes everything.   */
    /* ------------------------------------------------------------------ */

    /* --- Hostname --- */
    Serial_WriteString("INSTALL>hostname\n");
    char w_hostname[64];
    w_hostname[0] = '\0';
    {
        if (g_ansi) {
            tui_frame("Set hostname", "Enter a name for this machine (letters, digits, hyphens)");
            uint32_t mid  = g_rows / 2;
            uint32_t lcol = (g_cols / 2) > 20 ? (g_cols / 2) - 20 : 2;
            uint32_t fcol = lcol + 12;
            tui_at(lcol, mid, "Hostname:   ", C_FG, C_BG);
            {
                char blank[32]; for (int i=0;i<20;i++) blank[i]=' '; blank[20]='\0';
                tui_at(fcol, mid, blank, C_FG, C_BG);
            }
            tui_at(lcol, mid + 2, "Press Enter to accept (default: makar)", C_DIM, C_BG);
            {
                size_t len = 0;
                tui_at(fcol, mid, "_", C_TITLE, C_BG);
                while (1) {
                    unsigned char c = getkey();
                    if (c == '\n' || c == '\r') {
                        tui_at(fcol+(uint32_t)len, mid, " ", C_FG, C_BG);
                        break;
                    }
                    if (c == '\b' || c == 127) {
                        if (len) {
                            tui_at(fcol+(uint32_t)len, mid, " ", C_FG, C_BG);
                            len--;
                            tui_at(fcol+(uint32_t)len, mid, "_", C_TITLE, C_BG);
                        }
                        continue;
                    }
                    if (c < 0x20 || c > 0x7E) continue;
                    if (len < sizeof(w_hostname)-1) {
                        char ch[2] = {(char)c, '\0'};
                        tui_at(fcol+(uint32_t)len, mid, ch, C_FG, C_BG);
                        w_hostname[len++] = (char)c;
                        tui_at(fcol+(uint32_t)len, mid, "_", C_TITLE, C_BG);
                    }
                }
                w_hostname[len] = '\0';
            }
        } else {
            t_writestring("\nHostname (default: makar): ");
            readline(w_hostname, sizeof(w_hostname));
        }
        if (!w_hostname[0]) {
            w_hostname[0]='m'; w_hostname[1]='a'; w_hostname[2]='k';
            w_hostname[3]='a'; w_hostname[4]='r'; w_hostname[5]='\0';
        }
    }

    /* --- Accounts --- */
    Serial_WriteString("INSTALL>accounts\n");

    /* Every wizard choice lands in one install_params_t; the TUI then drives the
     * SAME shared engine (install_exec_*) the GUI installer uses, so the two
     * installers are functionally identical -- only the front-end differs. */
    install_params_t ip;
    for (unsigned _i = 0; _i < sizeof ip; _i++) ((unsigned char *)&ip)[_i] = 0;
    ip.drive        = (unsigned char)drive;
    ip.fs           = (fs == ROOTFS_FAT32) ? INSTALL_FS_FAT32 : INSTALL_FS_EXT2;
    ip.install_docs = (unsigned char)s_inst_docs;
    ip.install_src  = (unsigned char)s_inst_src;
    { int _h = 0; for (; w_hostname[_h] && _h < (int)sizeof(ip.hostname)-1; _h++)
          ip.hostname[_h] = w_hostname[_h]; ip.hostname[_h] = '\0'; }

    static const struct { const char *user; const char *prompt; int required; }
    acct_steps[] = {
        { "root", "Set root password", 1 },
        { NULL,   "Create a user",     0 },
    };

    for (int step = 0; step < 2; step++) {
        const char *fixed_user = acct_steps[step].user;
        const char *title      = acct_steps[step].prompt;
        int         required   = acct_steps[step].required;

        char username[64];
        char pass1[256], pass2[256];

        if (g_ansi) {
            tui_frame(title,
                      required ? "Enter a password for root  (Enter to skip)"
                               : "Y = create user   N / Enter = skip");

            uint32_t mid  = g_rows / 2;
            uint32_t lcol = (g_cols / 2) > 20 ? (g_cols / 2) - 20 : 2;
            uint32_t fcol = lcol + 14;

            if (!fixed_user) {
                tui_at(lcol, mid - 2, "Create an additional user? [y/N]", C_FG, C_BG);
                {
                    unsigned char ans = getkey();
                    if (ans != 'y' && ans != 'Y') goto next_acct;
                }
                tui_at(lcol, mid - 4, "Username:     ", C_FG, C_BG);
                {
                    char blank[32]; for (int i=0;i<20;i++) blank[i]=' '; blank[20]='\0';
                    tui_at(fcol, mid - 4, blank, C_FG, C_BG);
                }
                {
                    char blank[40]; for (int i=0;i<36;i++) blank[i]=' '; blank[36]='\0';
                    tui_at(lcol, mid - 2, blank, C_FG, C_BG);
                }
                {
                    size_t len = 0;
                    tui_at(fcol, mid-4, "_", C_TITLE, C_BG);
                    while (1) {
                        unsigned char c = getkey();
                        if (c == '\n' || c == '\r') {
                            tui_at(fcol+(uint32_t)len, mid-4, " ", C_FG, C_BG);
                            break;
                        }
                        if (c == 0x1B) { username[0]='\0'; break; }
                        if (c == '\b' || c == 127) {
                            if (len) {
                                tui_at(fcol+(uint32_t)len, mid-4, " ", C_FG, C_BG);
                                len--;
                                tui_at(fcol+(uint32_t)len, mid-4, "_", C_TITLE, C_BG);
                            }
                            continue;
                        }
                        if (c < 0x20 || c > 0x7E) continue;
                        if (len < sizeof(username)-1) {
                            char ch[2]={(char)c,'\0'};
                            tui_at(fcol+(uint32_t)len, mid-4, ch, C_FG, C_BG);
                            username[len++]=(char)c;
                            tui_at(fcol+(uint32_t)len, mid-4, "_", C_TITLE, C_BG);
                        }
                    }
                    username[len]='\0';
                }
                if (!username[0]) goto next_acct;
            } else {
                { int i = 0;
                  for (; fixed_user[i] && i < (int)sizeof(username)-1; i++)
                      username[i] = fixed_user[i];
                  username[i] = '\0'; }   /* terminate after the name, not at [63] */
            }

            uint32_t row_pass  = fixed_user ? mid - 2 : mid;
            uint32_t row_pass2 = row_pass + 2;
            uint32_t row_hint  = row_pass2 + 2;

            tui_at(lcol, row_pass,  "Password:     ", C_FG, C_BG);
            tui_at(lcol, row_pass2, "Confirm:      ", C_FG, C_BG);
            tui_at(lcol, row_hint,  "Press Enter to confirm", C_DIM, C_BG);

            char blanks[32]; for (int i=0;i<20;i++) blanks[i]=' '; blanks[20]='\0';
            tui_at(fcol, row_pass,  blanks, C_FG, C_BG);
            tui_at(fcol, row_pass2, blanks, C_FG, C_BG);

            readline_masked(pass1, sizeof(pass1), fcol, row_pass);
            if (!pass1[0] && required) {
                tui_at(lcol, row_hint, "Password unchanged (skipped).       ", C_DIM, C_BG);
                goto next_acct;
            }
            if (!pass1[0]) goto next_acct;

            readline_masked(pass2, sizeof(pass2), fcol, row_pass2);
            if (strcmp(pass1, pass2) != 0) {
                tui_at(lcol, row_hint, "Passwords do not match. Press a key.", C_WARN, C_BG);
                getkey();
                goto next_acct;
            }
        } else {
            if (!fixed_user) {
                t_writestring("\nCreate a user (blank to skip): ");
                readline(username, sizeof(username));
                if (!username[0]) goto next_acct;
            } else {
                { int i = 0;
                  for (; fixed_user[i] && i < (int)sizeof(username)-1; i++)
                      username[i] = fixed_user[i];
                  username[i] = '\0'; }   /* terminate after the name, not at [63] */
                t_writestring("\nSet root password (Enter to skip): ");
            }
            readline_masked(pass1, sizeof(pass1), 0, 0);
            if (!pass1[0]) goto next_acct;
            t_writestring("Confirm: ");
            readline_masked(pass2, sizeof(pass2), 0, 0);
            if (strcmp(pass1, pass2) != 0) {
                t_writestring("Passwords do not match.\n");
                goto next_acct;
            }
        }

        /* Stash the confirmed plaintext into the shared params; the engine
         * salts + hashes it into /etc/shadow (same path as the GUI installer). */
        if (step == 0) {                 /* root */
            int _i = 0; for (; pass1[_i] && _i < (int)sizeof(ip.root_pw)-1; _i++)
                ip.root_pw[_i] = pass1[_i];
            ip.root_pw[_i] = '\0';
        } else {                         /* additional user */
            int _i = 0; for (; username[_i] && _i < (int)sizeof(ip.user_name)-1; _i++)
                ip.user_name[_i] = username[_i];
            ip.user_name[_i] = '\0';
            _i = 0; for (; pass1[_i] && _i < (int)sizeof(ip.user_pw)-1; _i++)
                ip.user_pw[_i] = pass1[_i];
            ip.user_pw[_i] = '\0';
        }

        next_acct:;
    }

    /* ------------------------------------------------------------------ */
    /* Auto-login toggle — offer to skip the password prompt on boot.     */
    /* ------------------------------------------------------------------ */
    {
        /* Prefer the new user; fall back to root (if a root password was set). */
        const char *cand = ip.user_name[0] ? ip.user_name
                         : (ip.root_pw[0]  ? "root" : (const char *)0);

        if (cand) {
            char q[96]; size_t qp = 0;
            const char *pre = "Enable auto-login for '";
            for (const char *s = pre;  *s && qp < sizeof(q)-1; s++) q[qp++] = *s;
            for (const char *s = cand; *s && qp < sizeof(q)-1; s++) q[qp++] = *s;
            const char *suf = "'? [y/N]";
            for (const char *s = suf;  *s && qp < sizeof(q)-1; s++) q[qp++] = *s;
            q[qp] = '\0';

            unsigned char ans = 0;
            if (g_ansi) {
                tui_frame("Auto-login",
                          "Skip the password prompt and sign in automatically on boot?");
                uint32_t mid  = g_rows / 2;
                uint32_t lcol = (g_cols / 2) > 20 ? (g_cols / 2) - 20 : 2;
                tui_at(lcol, mid - 1, q, C_FG, C_BG);
                ans = getkey();
            } else {
                t_writestring(q); t_writestring(" ");
                char buf[8]; readline(buf, sizeof(buf));
                ans = (unsigned char)buf[0];
            }
            if (ans == 'y' || ans == 'Y') {
                int i = 0;
                for (; cand[i] && i < (int)sizeof(ip.autologin)-1; i++) ip.autologin[i] = cand[i];
                ip.autologin[i] = '\0';
            }
        }
    }

    /* ------------------------------------------------------------------ */
    /* Execute via the shared engine -- identical to the GUI installer.    */
    /* ------------------------------------------------------------------ */
    exec_screen("Installing Makar");
    tui_log("Preparing disk (partition + format + bootloader)...");
    if (install_exec_begin(&ip) < 0) {
        t_writestring("INSTALL: failed\n");
        if (g_ansi) {
            tui_frame("Installer", "Press a key to return to the shell");
            tui_center(6, "Installation FAILED.", C_WARN, C_BG);
            tui_center(8, "See the messages above; nothing was finalised.", C_DIM, C_BG);
            getkey();
        } else {
            t_writestring("\n=== Installation FAILED ===\n");
        }
        return;
    }
    {
        install_progress_t prog;
        prog.done = 0; prog.files = 0; prog.total = 0; prog.current[0] = '\0';
        while (install_exec_step(&prog) > 0) {
            unsigned pct = prog.total ? (prog.files * 100u) / prog.total : 100u;
            char st[80]; int _o = 0;
            const char *cc = "  ["; while (*cc) st[_o++] = *cc++;
            { uint32_t v=pct; char n[4]; int ni=0; if(!v)n[ni++]='0'; while(v){n[ni++]=(char)('0'+v%10);v/=10;} while(ni)st[_o++]=n[--ni]; }
            const char *pp = "%] "; while (*pp) st[_o++] = *pp++;
            for (const char *q = prog.current; *q && _o < (int)sizeof(st)-2; q++) st[_o++] = *q;
            st[_o] = '\0';
            tui_status(st);
        }
    }
    install_exec_finish(&ip);
    tui_status("");
    tui_log("Done.");

    t_writestring("INSTALL: complete ok\n");
    Serial_WriteString("[install] complete -- rebooting\n");
    if (g_ansi) {
        tui_frame("Installer", "Rebooting...");
        tui_center(6, "Installation complete!", C_OK, C_BG);
        tui_center(8, "Remove the install media. Rebooting in 3 seconds...", C_FG, C_BG);
    } else {
        t_writestring("\n=== Installation complete! Rebooting... ===\n");
    }
    uint32_t reboot_at = timer_get_ticks() + 300u; /* 3 seconds at 100 Hz */
    while (timer_get_ticks() < reboot_at)
        task_yield();
    acpi_reboot();
}
