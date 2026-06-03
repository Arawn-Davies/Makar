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
 * Rendering uses the VESA TTY paint primitives (vesa_tty_*), reserving the
 * bottom makmux status row.  In VGA-text fallback it degrades to a plain
 * numbered-prompt flow.  The shell repaints the screen (shell_restore_screen)
 * after installer_run() returns.
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
#define INST_MAX_FILE_SIZE    (8u * 1024u * 1024u)
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

/* Installed target info, set during do_install so the account-creation
 * wizard can remount the data partition without re-discovering it. */
static uint8_t  s_inst_drive;
static uint32_t s_inst_boot_lba;
static uint32_t s_inst_data_lba;
static int      s_inst_fs;

/* Optional component trees, chosen in the wizard (default: install both). */
static int      s_inst_docs = 1;   /* copy /docs to the target rootfs */
static int      s_inst_src  = 1;   /* copy /src  to the target rootfs */

/* ------------------------------------------------------------------------- */
/* Geometry                                                                  */
/* ------------------------------------------------------------------------- */

static int g_gui;        /* 1 = VESA TUI, 0 = VGA text fallback */
static uint32_t g_cols, g_rows;   /* usable area (status row excluded)       */

static void tui_geometry(void)
{
    g_gui = vesa_tty_is_ready();
    if (g_gui) {
        g_cols = vesa_tty_get_cols();
        g_rows = vesa_tty_usable_rows();
    } else {
        g_cols = 80;
        g_rows = 49;   /* 80x50 VGA text, minus status row */
    }
}

/* ------------------------------------------------------------------------- */
/* Low-level paint helpers (GUI mode only)                                   */
/* ------------------------------------------------------------------------- */

static void paint_fill(uint32_t bg)
{
    char blank[256];
    uint32_t n = (g_cols < 255) ? g_cols : 255;
    for (uint32_t i = 0; i < n; i++) blank[i] = ' ';
    blank[n] = '\0';
    for (uint32_t r = 0; r < g_rows; r++)
        vesa_tty_paint_string_at(0, r, blank, C_FG, bg);
}

static void tui_at(uint32_t col, uint32_t row, const char *s,
                   uint32_t fg, uint32_t bg)
{
    if (g_gui)
        vesa_tty_paint_string_at(col, row, s, fg, bg);
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
    paint_fill(C_BG);
    char bar[256];
    uint32_t n = (g_cols < 255) ? g_cols : 255;
    for (uint32_t i = 0; i < n; i++) bar[i] = ' ';
    bar[n] = '\0';
    vesa_tty_paint_string_at(0, 0, bar, C_SELFG, C_TITLE);
    tui_center(0, title, C_SELFG, C_TITLE);
    if (hint)
        tui_at(2, g_rows - 1, hint, C_DIM, C_BG);
}

/* ------------------------------------------------------------------------- */
/* Keyboard helpers                                                          */
/* ------------------------------------------------------------------------- */

static unsigned char getkey(void) { return keyboard_getchar(); }

/* Read a line (VGA fallback prompts / "type yes" confirm). Echoes to VGA. */
static void readline(char *buf, size_t max)
{
    size_t len = 0;
    while (1) {
        unsigned char c = getkey();
        if (c == '\n' || c == '\r') { t_putchar('\n'); break; }
        if (c == '\b') { if (len) { len--; t_backspace(); } continue; }
        if (c < 0x20 || c > 0x7E) continue;
        if (len < max - 1) { buf[len++] = (char)c; t_putchar((char)c); }
    }
    buf[len] = '\0';
}

/* Masked password readline: echoes '*' per character with a '_' cursor.
 * GUI mode paints at (field_col, row); VGA mode echoes to terminal cursor. */
static void readline_masked(char *buf, size_t max,
                            uint32_t field_col, uint32_t row)
{
    size_t len = 0;

    /* Draw initial cursor */
    if (g_gui)
        vesa_tty_paint_string_at(field_col, row, "_", C_TITLE, C_BG);

    while (1) {
        unsigned char c = getkey();
        if (c == '\n' || c == '\r') {
            /* Clear cursor on commit */
            if (g_gui)
                vesa_tty_paint_string_at(field_col + (uint32_t)len, row, " ", C_FG, C_BG);
            break;
        }
        if (c == '\b' || c == 127) {
            if (len) {
                /* Erase cursor at current pos */
                if (g_gui)
                    vesa_tty_paint_string_at(field_col + (uint32_t)len, row, " ", C_FG, C_BG);
                len--;
                /* Erase the star that was at len */
                if (g_gui)
                    vesa_tty_paint_string_at(field_col + (uint32_t)len, row, "_", C_TITLE, C_BG);
                else
                    t_backspace();
            }
            continue;
        }
        if (c < 0x20 || c > 0x7E) continue;
        if (len < max - 1) {
            /* Overwrite cursor with star, advance cursor */
            if (g_gui) {
                vesa_tty_paint_string_at(field_col + (uint32_t)len, row, "*", C_FG, C_BG);
            } else {
                t_putchar('*');
            }
            buf[len++] = (char)c;
            if (g_gui)
                vesa_tty_paint_string_at(field_col + (uint32_t)len, row, "_", C_TITLE, C_BG);
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
    if (!g_gui) {
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

    int sel = 0;
    uint32_t top = 4;
    while (1) {
        tui_frame(title, hint ? hint : "Up/Down to move  Enter to select  Esc to cancel");
        for (int i = 0; i < n; i++) {
            uint32_t row = top + (uint32_t)i * 2;
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
                tui_at(8, row + 1, descs[i], C_DIM, C_BG);
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
    if (g_gui) {
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
    if (!g_gui) return;
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
    if (g_gui) {
        vesa_tty_clear();
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

    if (g_gui) {
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
    if (g_gui) {
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

/* Recursively mirror an ISO directory tree onto the rootfs (BFS).  Paths are
 * identical on both sides (same layout), so one path serves source + dest.
 * Logs one step line, then updates a running file count on the status line so
 * the box doesn't fill with hundreds of per-file lines. */
static void copy_tree(const char *root)
{
    char line[LOG_W];
    str_u(line, "Copying ", 0);   /* placeholder, rebuild below */
    int o = 0; const char *p = "Copying "; while (*p) line[o++] = *p++;
    p = root; while (*p) line[o++] = *p++;
    const char *t = " ..."; while (*t) line[o++] = *t++; line[o] = '\0';
    tui_log(line);

    uint32_t files = 0;
    rfs_mkdir(root);
    q_reset();
    q_push(root);
    while (!q_empty()) {
        char dir[PATH_MAX_];
        strncpy(dir, q_pop(), PATH_MAX_ - 1);
        dir[PATH_MAX_ - 1] = '\0';

        s_nents = 0;
        iso9660_complete(g_cd, dir, "", enum_cb, NULL);

        for (int i = 0; i < s_nents; i++) {
            char child[PATH_MAX_];
            int co = 0;
            const char *d = dir;
            while (*d && co < PATH_MAX_ - 1) child[co++] = *d++;
            if (!(co == 1 && child[0] == '/') && co < PATH_MAX_ - 1)
                child[co++] = '/';
            const char *nm = s_ents[i].name;
            while (*nm && co < PATH_MAX_ - 1) child[co++] = *nm++;
            child[co] = '\0';

            if (s_ents[i].is_dir) {
                rfs_mkdir(child);
                q_push(child);
            } else {
                copy_file(child, child);
                files++;
                if ((files % 10) == 0) {
                    char st[64];
                    str_u(st, "  copied ", files);
                    int so = (int)strlen(st);
                    const char *f = " files"; while (*f) st[so++] = *f++; st[so] = '\0';
                    tui_status(st);
                }
            }
        }
    }
    char done[64];
    str_u(done, "  done - ", files);
    int do_ = (int)strlen(done);
    const char *f = " files"; while (*f) done[do_++] = *f++; done[do_] = '\0';
    tui_log(done);
    tui_status("");
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

static const char limine_conf[] =
    "# Generated by the Makar installer.\n"
    "timeout: 3\n"
    "\n"
    "/Makar OS\n"
    "    protocol: multiboot2\n"
    "    path: boot():/boot/makar.kernel\n";

/* Append "<prefix> (rc=<n>)" to the installer log -- detailed failure
 * context so a post-mortem `cat /log/install.log` records exactly which
 * step failed and the backend return code.  Buf is generous (`tui_log`
 * truncates at LOG_W). */
static void tui_log_rc(const char *prefix, int rc)
{
    char buf[160];
    int  i = 0;
    while (prefix[i] && i < 120) { buf[i] = prefix[i]; i++; }
    /* " (rc=<int>)" */
    const char *suf = " (rc=";
    for (int j = 0; suf[j] && i < 154; j++) buf[i++] = suf[j];
    int n = rc;
    if (n < 0) { buf[i++] = '-'; n = -n; }
    char dig[12]; int dn = 0;
    if (n == 0) dig[dn++] = '0';
    while (n) { dig[dn++] = (char)('0' + (n % 10)); n /= 10; }
    while (dn--) buf[i++] = dig[dn];
    buf[i++] = ')';
    buf[i]   = '\0';
    tui_log(buf);
}

static int do_install(uint8_t drive, uint32_t disk_sectors, int fs)
{
    uint32_t boot_lba = 0, boot_count = 0;
    uint32_t data_lba = 0, data_count = 0;

    exec_screen("Installing Makar");

    tui_log("Partitioning drive...");
    int prc = partition_whole_disk(drive, disk_sectors, fs,
                                   &boot_lba, &boot_count,
                                   &data_lba, &data_count);
    if (prc != 0) {
        tui_log_rc("  ERROR: failed to write partition table.", prc);
        return -1;
    }

    /* ---- Partition 1: FAT32 boot (kernel + limine stage 3) ---- */

    tui_log("Formatting boot partition (FAT32, 34 MiB)...");
    int rc;
    if ((rc = fat32_mkfs(drive, boot_lba, boot_count)) != 0) {
        tui_log_rc("  ERROR: boot mkfs failed (need >= 33 MiB for FAT32).", rc);
        return -2;
    }

    /* Boot files are copied after the remaining wizard inputs have been
     * collected.  This phase only prepares the filesystem. */

    /* ---- Partition 2: data (apps / docs / src) ---- */

    tui_log(fs == ROOTFS_EXT2 ? "Formatting data partition (ext2)..."
                               : "Formatting data partition (FAT32)...");
    int mk = (fs == ROOTFS_EXT2) ? ext2_mkfs(drive, data_lba, data_count)
                                  : fat32_mkfs(drive, data_lba, data_count);
    if (mk != 0) {
        tui_log_rc(fs == ROOTFS_EXT2
                       ? "  ERROR: data mkfs (ext2) failed."
                       : "  ERROR: data mkfs (FAT32) failed.", mk);
        return -4;
    }

    tui_log("Mounting data partition...");
    if (fat32_mounted()) fat32_unmount();
    int mnt = (fs == ROOTFS_EXT2) ? ext2_mount(drive, data_lba)
                                   : fat32_mount(drive, data_lba);
    if (mnt != 0) {
        tui_log_rc(fs == ROOTFS_EXT2 ? "  ERROR: data mount (ext2) failed."
                                      : "  ERROR: data mount (FAT32) failed.", mnt);
        return -5;
    }
    g_root_fs = fs;
    /* Record for the post-install account-creation step. */
    s_inst_drive    = drive;
    s_inst_boot_lba = boot_lba;
    s_inst_data_lba = data_lba;
    s_inst_fs       = fs;

    /* Data partition contents are copied after the remaining wizard inputs
     * have been collected, so the installer does not spend time mirroring
     * large trees before hostname/account prompts. */
    if (fs == ROOTFS_EXT2) ext2_unmount(); else fat32_unmount();

    /* ---- limine MBR install ---- */

    tui_log("Installing limine bootloader...");
    /* The MBR boot code + stage2 come from limine-hdd.bin (mirrors the host
     * bios-install, which embeds binary_limine_hdd_bin_data); limine-bios.sys
     * is stage3 and lives on the FAT32 /mnt/boot partition (/limine, above). */
    uint32_t sys_sz = 0;
    if (iso9660_read_file(g_cd, LIMINE_HDD_ISO_PATH, s_filebuf,
                          INST_MAX_FILE_SIZE, &sys_sz) != 0 || sys_sz <= 512) {
        tui_log("  ERROR: cannot read limine-hdd.bin from CD.");
        return -6;
    }
    int li = limine_install_mbr(drive, s_filebuf, sys_sz);
    if (li != 0) {
        char e[48];
        str_u(e, "  ERROR: limine install failed (", (uint32_t)(-li));
        int eo = (int)strlen(e); e[eo++] = ')'; e[eo] = '\0';
        tui_log(e);
        return -7;
    }

    tui_log("Done.");
    return 0;
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
    if (g_gui) {
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
        if (g_gui) { tui_frame("Installer", "Press a key"); tui_center(6, "No ISO9660 CD-ROM source found.", C_WARN, C_BG); getkey(); }
        else t_writestring("Error: no ISO9660 CD-ROM source found.\n");
        return;
    }
    g_cd = (uint8_t)cd;

    int n = build_drive_list();
    if (n == 0) {
        if (g_gui) { tui_frame("Installer", "Press a key"); tui_center(6, "No ATA target drives found.", C_WARN, C_BG); getkey(); }
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
        "Apps / docs / src land on an ext2 data partition.\n"
        "  A separate FAT32 boot partition (34 MiB) holds the\n"
        "  kernel and Limine stage 3 (limine-bios.sys).",
        "Apps / docs / src land on a FAT32 data partition.\n"
        "  A separate FAT32 boot partition (34 MiB) holds the\n"
        "  kernel and Limine stage 3 (limine-bios.sys)." };
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
        if (g_gui) {
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
        if (!g_gui) t_writestring("Installation cancelled.\n");
        return;
    }

    /* ---- Optional components: docs + source trees (default: install both) ---- */
    Serial_WriteString("INSTALL>components\n");
    {
        unsigned char a1 = 0, a2 = 0;
        if (g_gui) {
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
        if (g_gui) {
            tui_frame("Set hostname", "Enter a name for this machine (letters, digits, hyphens)");
            uint32_t mid  = g_rows / 2;
            uint32_t lcol = (g_cols / 2) > 20 ? (g_cols / 2) - 20 : 2;
            uint32_t fcol = lcol + 12;
            tui_at(lcol, mid, "Hostname:   ", C_FG, C_BG);
            {
                char blank[32]; for (int i=0;i<20;i++) blank[i]=' '; blank[20]='\0';
                vesa_tty_paint_string_at(fcol, mid, blank, C_FG, C_BG);
            }
            tui_at(lcol, mid + 2, "Press Enter to accept (default: makar)", C_DIM, C_BG);
            {
                size_t len = 0;
                vesa_tty_paint_string_at(fcol, mid, "_", C_TITLE, C_BG);
                while (1) {
                    unsigned char c = getkey();
                    if (c == '\n' || c == '\r') {
                        vesa_tty_paint_string_at(fcol+(uint32_t)len, mid, " ", C_FG, C_BG);
                        break;
                    }
                    if (c == '\b' || c == 127) {
                        if (len) {
                            vesa_tty_paint_string_at(fcol+(uint32_t)len, mid, " ", C_FG, C_BG);
                            len--;
                            vesa_tty_paint_string_at(fcol+(uint32_t)len, mid, "_", C_TITLE, C_BG);
                        }
                        continue;
                    }
                    if (c < 0x20 || c > 0x7E) continue;
                    if (len < sizeof(w_hostname)-1) {
                        char ch[2] = {(char)c, '\0'};
                        vesa_tty_paint_string_at(fcol+(uint32_t)len, mid, ch, C_FG, C_BG);
                        w_hostname[len++] = (char)c;
                        vesa_tty_paint_string_at(fcol+(uint32_t)len, mid, "_", C_TITLE, C_BG);
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

    /* Per-account wizard state; shadow lines built in memory. */
    struct {
        char username[64];
        char shadow_line[256];
        size_t shadow_len;
    } w_accts[2];
    int w_naccts = 0;
    char w_autologin[64];   /* username to auto-log-in, or "" for none */
    w_autologin[0] = '\0';

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

        if (g_gui) {
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
                    vesa_tty_paint_string_at(fcol, mid - 4, blank, C_FG, C_BG);
                }
                {
                    char blank[40]; for (int i=0;i<36;i++) blank[i]=' '; blank[36]='\0';
                    vesa_tty_paint_string_at(lcol, mid - 2, blank, C_FG, C_BG);
                }
                {
                    size_t len = 0;
                    vesa_tty_paint_string_at(fcol, mid-4, "_", C_TITLE, C_BG);
                    while (1) {
                        unsigned char c = getkey();
                        if (c == '\n' || c == '\r') {
                            vesa_tty_paint_string_at(fcol+(uint32_t)len, mid-4, " ", C_FG, C_BG);
                            break;
                        }
                        if (c == 0x1B) { username[0]='\0'; break; }
                        if (c == '\b' || c == 127) {
                            if (len) {
                                vesa_tty_paint_string_at(fcol+(uint32_t)len, mid-4, " ", C_FG, C_BG);
                                len--;
                                vesa_tty_paint_string_at(fcol+(uint32_t)len, mid-4, "_", C_TITLE, C_BG);
                            }
                            continue;
                        }
                        if (c < 0x20 || c > 0x7E) continue;
                        if (len < sizeof(username)-1) {
                            char ch[2]={(char)c,'\0'};
                            vesa_tty_paint_string_at(fcol+(uint32_t)len, mid-4, ch, C_FG, C_BG);
                            username[len++]=(char)c;
                            vesa_tty_paint_string_at(fcol+(uint32_t)len, mid-4, "_", C_TITLE, C_BG);
                        }
                    }
                    username[len]='\0';
                }
                if (!username[0]) goto next_acct;
            } else {
                for (int i=0; fixed_user[i] && i<(int)sizeof(username)-1; i++)
                    username[i] = fixed_user[i];
                username[sizeof(username)-1] = '\0';
            }

            uint32_t row_pass  = fixed_user ? mid - 2 : mid;
            uint32_t row_pass2 = row_pass + 2;
            uint32_t row_hint  = row_pass2 + 2;

            tui_at(lcol, row_pass,  "Password:     ", C_FG, C_BG);
            tui_at(lcol, row_pass2, "Confirm:      ", C_FG, C_BG);
            tui_at(lcol, row_hint,  "Press Enter to confirm", C_DIM, C_BG);

            char blanks[32]; for (int i=0;i<20;i++) blanks[i]=' '; blanks[20]='\0';
            vesa_tty_paint_string_at(fcol, row_pass,  blanks, C_FG, C_BG);
            vesa_tty_paint_string_at(fcol, row_pass2, blanks, C_FG, C_BG);

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
                for (int i=0; fixed_user[i] && i<(int)sizeof(username)-1; i++)
                    username[i] = fixed_user[i];
                username[sizeof(username)-1] = '\0';
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

        /* Build shadow line in memory — no disk I/O here. */
        if (w_naccts < 2) {
            /* Copy username */
            size_t ui = 0;
            while (username[ui] && ui < sizeof(w_accts[0].username)-1) {
                w_accts[w_naccts].username[ui] = username[ui]; ui++;
            }
            w_accts[w_naccts].username[ui] = '\0';

            /* Compute salt + hash */
            char salt[17], hash[17];
            {
                uint32_t ticks = timer_get_ticks();
                uint32_t rtcsec = 0; rtc_unix_time(&rtcsec);
                uint8_t seed[8];
                seed[0]=(uint8_t)ticks;       seed[1]=(uint8_t)(ticks>>8);
                seed[2]=(uint8_t)(ticks>>16);  seed[3]=(uint8_t)(ticks>>24);
                seed[4]=(uint8_t)rtcsec;       seed[5]=(uint8_t)(rtcsec>>8);
                seed[6]=(uint8_t)(rtcsec>>16); seed[7]=(uint8_t)(rtcsec>>24);
                microhash_hex16(seed, 8, salt);
            }
            {
                size_t plen = strlen(pass1);
                if (plen > 256) plen = 256;
                uint8_t combined[16 + 256];
                for (int i = 0; i < 16; i++) combined[i] = (uint8_t)salt[i];
                for (size_t i = 0; i < plen; i++) combined[16+i] = (uint8_t)pass1[i];
                microhash_hex16(combined, 16 + plen, hash);
            }

            /* "username:$mh$<salt16>$<hash16>:::::::\n" */
            char *ln = w_accts[w_naccts].shadow_line;
            size_t pos = 0;
            for (size_t i = 0; username[i] && pos < 64; i++) ln[pos++] = username[i];
            ln[pos++]=':'; ln[pos++]='$'; ln[pos++]='m';
            ln[pos++]='h'; ln[pos++]='$';
            for (int i=0;i<16;i++) ln[pos++]=salt[i];
            ln[pos++]='$';
            for (int i=0;i<16;i++) ln[pos++]=hash[i];
            const char *tail = ":::::::\n";
            for (int i=0;tail[i];i++) ln[pos++]=tail[i];
            w_accts[w_naccts].shadow_len = pos;
            w_naccts++;
        }

        next_acct:;
    }

    /* ------------------------------------------------------------------ */
    /* Auto-login toggle — offer to skip the password prompt on boot.     */
    /* ------------------------------------------------------------------ */
    {
        /* Prefer the first non-root account; fall back to root. */
        const char *cand = (const char *)0;
        for (int i = 0; i < w_naccts; i++) {
            const char *u = w_accts[i].username;
            int is_root = (u[0]=='r'&&u[1]=='o'&&u[2]=='o'&&u[3]=='t'&&u[4]=='\0');
            if (!is_root) { cand = u; break; }
        }
        if (!cand && w_naccts > 0) cand = w_accts[0].username;

        if (cand) {
            char q[96]; size_t qp = 0;
            const char *pre = "Enable auto-login for '";
            for (const char *s = pre;  *s && qp < sizeof(q)-1; s++) q[qp++] = *s;
            for (const char *s = cand; *s && qp < sizeof(q)-1; s++) q[qp++] = *s;
            const char *suf = "'? [y/N]";
            for (const char *s = suf;  *s && qp < sizeof(q)-1; s++) q[qp++] = *s;
            q[qp] = '\0';

            unsigned char ans = 0;
            if (g_gui) {
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
                size_t i = 0;
                while (cand[i] && i < sizeof(w_autologin)-1) { w_autologin[i] = cand[i]; i++; }
                w_autologin[i] = '\0';
            }
        }
    }

    /* ------------------------------------------------------------------ */
    /* Disk preparation: no partitioning/formatting/copying starts until */
    /* all installer input has been collected.                           */
    /* ------------------------------------------------------------------ */

    s_filebuf = (uint8_t *)kmalloc(INST_MAX_FILE_SIZE);
    if (!s_filebuf) {
        t_writestring("Error: out of memory for transfer buffer.\n");
        return;
    }

    int rc = do_install((uint8_t)drive, hdd->size, fs);

    if (rc != 0) {
        t_writestring("INSTALL: failed\n");
        kfree(s_filebuf);
        s_filebuf = NULL;
        if (g_gui) {
            tui_frame("Installer", "Press a key to return to the shell");
            tui_center(6, "Installation FAILED.", C_WARN, C_BG);
            tui_center(8, "See the messages above; nothing was finalised.", C_DIM, C_BG);
            getkey();
        } else {
            t_writestring("\n=== Installation FAILED ===\n");
        }
        return;
    }
    Serial_WriteString("INSTALL: disk prepared ok\n");

    /* ------------------------------------------------------------------ */
    /* Final disk writes: boot files, account config, and optional trees. */
    /* All file copying happens here, after the remaining wizard inputs. */
    /* ------------------------------------------------------------------ */
    {
        tui_log("Mounting boot partition...");
        if (fat32_mounted()) fat32_unmount();
        int bm = fat32_mount(s_inst_drive, s_inst_boot_lba);
        if (bm == 0) {
            g_root_fs = ROOTFS_FAT32;

            tui_log("Creating boot directory tree...");
            int brc;
            if ((brc = fat32_mkdir("/boot")) != 0)
                tui_log_rc("  WARNING: mkdir /boot failed.", brc);
            if ((brc = fat32_mkdir("/limine")) != 0)
                tui_log_rc("  WARNING: mkdir /limine failed.", brc);

            tui_log("Copying kernel...");
            copy_one("/boot/makar.kernel", "/boot/makar.kernel");

            tui_log("Copying limine-bios.sys...");
            copy_one(LIMINE_SYS_ISO_PATH, "/limine/limine-bios.sys");

            tui_log("Writing limine.conf...");
            if ((brc = rfs_write("/limine/limine.conf", limine_conf,
                                  (uint32_t)(sizeof(limine_conf) - 1))) != 0)
                tui_log_rc("  WARNING: failed to write limine.conf.", brc);

            fat32_unmount();
        } else {
            tui_log_rc("  WARNING: boot mount failed during final copy.", bm);
        }
    }

    {
        int mnt = (s_inst_fs == ROOTFS_EXT2)
            ? ext2_mount(s_inst_drive, s_inst_data_lba)
            : fat32_mount(s_inst_drive, s_inst_data_lba);

        if (mnt == 0) {
            g_root_fs = s_inst_fs;

            rfs_mkdir("/etc");

            /* /etc/hostname */
            {
                size_t hlen = strlen(w_hostname);
                char hfile[66];
                for (size_t i = 0; i < hlen; i++) hfile[i] = w_hostname[i];
                hfile[hlen] = '\n'; hfile[hlen+1] = '\0';
                rfs_write("/etc/hostname", hfile, (uint32_t)(hlen + 1));
                Serial_WriteString("[install] hostname: ");
                Serial_WriteString(w_hostname);
                Serial_WriteString("\n");
            }

            /* /etc/autologin — written only if the operator opted in. */
            if (w_autologin[0]) {
                char albuf[66];
                size_t al = 0;
                while (w_autologin[al] && al < sizeof(albuf) - 2) {
                    albuf[al] = w_autologin[al]; al++;
                }
                albuf[al++] = '\n';
                albuf[al]   = '\0';
                rfs_write("/etc/autologin", albuf, (uint32_t)al);
                Serial_WriteString("[install] autologin: ");
                Serial_WriteString(w_autologin);
                Serial_WriteString("\n");
            }

            /* /etc/shadow — concatenate all entries */
            if (w_naccts > 0) {
                static char shadow_buf[512];
                size_t spos = 0;
                for (int i = 0; i < w_naccts; i++) {
                    size_t llen = w_accts[i].shadow_len;
                    if (spos + llen < sizeof(shadow_buf)) {
                        for (size_t j = 0; j < llen; j++)
                            shadow_buf[spos++] = w_accts[i].shadow_line[j];
                        Serial_WriteString("[install] shadow written for: ");
                        Serial_WriteString(w_accts[i].username);
                        Serial_WriteString("\n");
                    }
                }
                rfs_write("/etc/shadow", shadow_buf, (uint32_t)spos);
            }

            /* Home directories + default .makshrc */
            rfs_mkdir("/root");
            rfs_mkdir("/home");
            for (int i = 0; i < w_naccts; i++) {
                int is_root = (w_accts[i].username[0]=='r' &&
                               w_accts[i].username[1]=='o' &&
                               w_accts[i].username[2]=='o' &&
                               w_accts[i].username[3]=='t' &&
                               w_accts[i].username[4]=='\0');

                /* Compute home dir path */
                char hdir[80];
                size_t p = 0;
                if (is_root) {
                    hdir[p++]='/'; hdir[p++]='r'; hdir[p++]='o';
                    hdir[p++]='o'; hdir[p++]='t'; hdir[p]='\0';
                } else {
                    hdir[p++]='/'; hdir[p++]='h'; hdir[p++]='o';
                    hdir[p++]='m'; hdir[p++]='e'; hdir[p++]='/';
                    for (size_t j = 0; w_accts[i].username[j] && p < sizeof(hdir)-1; j++)
                        hdir[p++] = w_accts[i].username[j];
                    hdir[p] = '\0';
                    rfs_mkdir(hdir);
                }
                Serial_WriteString("[install] homedir: ");
                Serial_WriteString(hdir);
                Serial_WriteString("\n");

                /* Write ~/.makshrc with defaults */
                char rc_path[88];
                p = 0;
                for (size_t j = 0; hdir[j] && p < sizeof(rc_path)-10; j++)
                    rc_path[p++] = hdir[j];
                rc_path[p++]='/'; rc_path[p++]='.'; rc_path[p++]='m';
                rc_path[p++]='a'; rc_path[p++]='k'; rc_path[p++]='s';
                rc_path[p++]='h'; rc_path[p++]='r'; rc_path[p++]='c';
                rc_path[p]='\0';

                /* Default .makshrc content */
                static const char makrc_root[] =
                    "# ~/.makshrc -- sourced by sh.elf on login\n"
                    "PATH=/apps:/bin\n";
                static const char makrc_user[] =
                    "# ~/.makshrc -- sourced by sh.elf on login\n"
                    "PATH=/apps:/bin\n";
                const char *rc_content = is_root ? makrc_root : makrc_user;
                size_t rc_len = 0;
                while (rc_content[rc_len]) rc_len++;
                rfs_write(rc_path, rc_content, (uint32_t)rc_len);
                Serial_WriteString("[install] makshrc: ");
                Serial_WriteString(rc_path);
                Serial_WriteString("\n");

                /* Write ~/.vixrc enabling line numbers by default.  vix
                 * itself ships with line numbers OFF; this rc opts each
                 * installed account into them (Ctrl-N toggles at runtime). */
                char vix_path[88];
                p = 0;
                for (size_t j = 0; hdir[j] && p < sizeof(vix_path) - 8; j++)
                    vix_path[p++] = hdir[j];
                vix_path[p++]='/'; vix_path[p++]='.'; vix_path[p++]='v';
                vix_path[p++]='i'; vix_path[p++]='x'; vix_path[p++]='r';
                vix_path[p++]='c'; vix_path[p]='\0';
                static const char vixrc[] =
                    "\" ~/.vixrc -- read by vix on startup\n"
                    "set linenumbers\n";
                size_t vix_len = 0;
                while (vixrc[vix_len]) vix_len++;
                rfs_write(vix_path, vixrc, (uint32_t)vix_len);
                Serial_WriteString("[install] vixrc: ");
                Serial_WriteString(vix_path);
                Serial_WriteString("\n");

                /* Write ~/.sbrc -- statusbar.elf layout (foreground command
                 * left, resources right).  Sections: left/center/right + widgets. */
                char sb_path[88];
                p = 0;
                for (size_t j = 0; hdir[j] && p < sizeof(sb_path) - 7; j++)
                    sb_path[p++] = hdir[j];
                sb_path[p++]='/'; sb_path[p++]='.'; sb_path[p++]='s';
                sb_path[p++]='b'; sb_path[p++]='r'; sb_path[p++]='c';
                sb_path[p]='\0';
                static const char sbrc[] =
                    "# ~/.sbrc -- statusbar layout: <section> <widgets...>\n"
                    "# widgets: hostname user date time datetime uptime\n"
                    "#          cpu mem rootfs command tabs\n"
                    "left command\n"
                    "center tabs\n"
                    "right cpu mem rootfs time\n";
                size_t sb_len = 0;
                while (sbrc[sb_len]) sb_len++;
                rfs_write(sb_path, sbrc, (uint32_t)sb_len);
                Serial_WriteString("[install] sbrc: ");
                Serial_WriteString(sb_path);
                Serial_WriteString("\n");
            }

            copy_tree("/apps");
            if (s_inst_docs) copy_tree("/docs");
            if (s_inst_src)  copy_tree("/src");
            copy_tree("/usr");

            /* /bin is the scratch + output directory the in-OS kernel rebuild
             * (rebuild-kernel.sh) writes to.  tmpfs has no subdirectories, so
             * the rebuild can't use /tmp; it needs a real writable dir on the
             * rootfs.  Create it empty here so the first `mkdir /bin/ktcc` from
             * the script doesn't try to create a directory inside a missing
             * parent.  No source tree to copy -- /bin lives only on the target. */
            if (rfs_mkdir("/bin") != 0)
                tui_log("  WARNING: mkdir /bin on rootfs failed (rebuild-kernel will need it).");

            if (s_inst_fs == ROOTFS_EXT2) ext2_unmount();
            else fat32_unmount();
        } else {
            Serial_WriteString("[install] WARNING: could not mount data fs for post-install writes\n");
        }
    }
    kfree(s_filebuf);
    s_filebuf = NULL;

    /* ------------------------------------------------------------------ */
    /* Done                                                                 */
    /* ------------------------------------------------------------------ */

    t_writestring("INSTALL: complete ok\n");
    Serial_WriteString("[install] complete — rebooting\n");
    if (g_gui) {
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
