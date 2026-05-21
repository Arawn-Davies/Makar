/*
 * cfdisk.c - curses-style full-screen MBR partition editor (userland).
 *
 * Modelled on util-linux cfdisk: a partition/free-space table with
 * arrow-key row selection and a bottom action bar driven by left/right
 * arrows + Enter.  MBR primary-only (the four entries at offset 0x1BE);
 * the scriptable line-driven editor lives in fdisk.elf.
 *
 * Clean-room: modelled on util-linux cfdisk's screen layout and key flow,
 * but written from scratch against Makar's syscalls -- no upstream source
 * referenced or copied.  Slated for replacement by a real libfdisk-backed
 * port once a POSIX libc (uClibc-ng) lands.
 *
 * Renders through the same full-screen syscall set as vix.elf
 * (sys_putch_at / sys_set_cursor / sys_tty_clear / sys_term_size /
 * sys_getkey).  Disk I/O goes through a /dev block-device fd
 * (sys_open / sys_read / sys_write / sys_lseek).
 */

#include "syscall.h"

#define SECTOR_SIZE   512u
#define PART_OFFSET   0x1BE
#define PART_ENTRY    16
#define NUM_PARTS     4
#define ALIGN_SECT    2048u            /* 1 MiB alignment / embed gap      */
#define SECT_PER_MIB  2048u

/* Colours (fg | bg<<4). */
#define CLR_BG      VGA_CLR(VGA_LGREY, VGA_BLUE)
#define CLR_TITLE   VGA_CLR(VGA_WHITE, VGA_BLUE)
#define CLR_HEAD    VGA_CLR(VGA_YELLOW, VGA_BLUE)
#define CLR_ROW     VGA_CLR(VGA_LGREY, VGA_BLUE)
#define CLR_ROWSEL  VGA_CLR(VGA_BLUE, VGA_LGREY)
#define CLR_BTN     VGA_CLR(VGA_LGREY, VGA_BLUE)
#define CLR_BTNSEL  VGA_CLR(VGA_BLUE, VGA_LGREY)
#define CLR_STATUS  VGA_CLR(VGA_WHITE, VGA_BLUE)
#define CLR_WARN    VGA_CLR(VGA_WHITE, VGA_RED)

#define MAX_COLS    160
#define MAX_ROWS    64

/* ---- tiny libc ---------------------------------------------------------- */

static unsigned int ustrlen(const char *s) { unsigned int n = 0; while (s[n]) n++; return n; }

static void ucopy(char *d, const char *s) { while ((*d++ = *s++)) {} }

/* unsigned -> decimal string; returns length. */
static int utoa(unsigned int v, char *out)
{
    char t[12]; int n = 0;
    if (v == 0) { out[0] = '0'; out[1] = '\0'; return 1; }
    while (v && n < 12) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    int i; for (i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = '\0';
    return n;
}

static unsigned int parse_uint(const char *s)
{
    unsigned int v = 0;
    while (*s == ' ') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (*s) {
            char c = *s++; unsigned int d;
            if (c >= '0' && c <= '9') d = (unsigned int)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (unsigned int)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (unsigned int)(c - 'A' + 10);
            else break;
            v = v * 16u + d;
        }
        return v;
    }
    while (*s >= '0' && *s <= '9') v = v * 10u + (unsigned int)(*s++ - '0');
    return v;
}

/* Always hex (optional 0x prefix).  Partition type codes are hex by
 * convention (the prompt says so), but operators type bare "83" expecting
 * 0x83 (Linux), not decimal 83 (= 0x53 "Unknown"). */
static unsigned int parse_hex(const char *s)
{
    unsigned int v = 0;
    while (*s == ' ') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s++; unsigned int d;
        if (c >= '0' && c <= '9') d = (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned int)(c - 'A' + 10);
        else break;
        v = v * 16u + d;
    }
    return v;
}

/* ---- MBR access (little-endian) ---------------------------------------- */

static unsigned char mbr[SECTOR_SIZE];

static unsigned char *entry(int i) { return &mbr[PART_OFFSET + i * PART_ENTRY]; }

static unsigned int le32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void wr32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v);       p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static const char *type_name(unsigned char t)
{
    switch (t) {
    case 0x00: return "Free";
    case 0x01: return "FAT12";
    case 0x04: return "FAT16<32M";
    case 0x05: return "Extended";
    case 0x06: return "FAT16";
    case 0x07: return "NTFS/exFAT";
    case 0x0B: return "FAT32 CHS";
    case 0x0C: return "FAT32 LBA";
    case 0x0E: return "FAT16 LBA";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux/ext2";
    case 0xEE: return "GPT prot";
    case 0xEF: return "EFI System";
    default:   return "Unknown";
    }
}

/* ---- screen state ------------------------------------------------------- */

static int g_cols, g_rows;
static const char *g_dev = "/dev/hda";
static int   g_fd = -1;
static unsigned int g_total;     /* total disk sectors */
static int   g_dirty;

static tty_cell_t g_cells[MAX_COLS * MAX_ROWS];
static int        g_ncells;

static void put_at(int col, int row, char c, unsigned char clr)
{
    if (col < 0 || col >= g_cols || row < 0 || row >= g_rows) return;
    if (g_ncells < (int)(sizeof(g_cells) / sizeof(g_cells[0]))) {
        g_cells[g_ncells].col = (unsigned char)col;
        g_cells[g_ncells].row = (unsigned char)row;
        g_cells[g_ncells].ch  = (unsigned char)c;
        g_cells[g_ncells].clr = clr;
        g_ncells++;
    }
}

static void put_str(int col, int row, const char *s, unsigned char clr)
{
    for (int i = 0; s[i]; i++) put_at(col + i, row, s[i], clr);
}

/* Paint a fixed-width field, space-padded, at (col,row). */
static void put_field(int col, int row, const char *s, int width, unsigned char clr)
{
    int i = 0;
    for (; s[i] && i < width; i++) put_at(col + i, row, s[i], clr);
    for (; i < width; i++)         put_at(col + i, row, ' ', clr);
}

static void fill_bg(void)
{
    for (int r = 0; r < g_rows; r++)
        for (int c = 0; c < g_cols; c++)
            put_at(c, r, ' ', CLR_BG);
}

static void flush(void)
{
    sys_putch_at(g_cells, (unsigned int)g_ncells);
    g_ncells = 0;
}

/* ---- display-list model ------------------------------------------------- */
/*
 * The table interleaves the (sorted) populated primary partitions with the
 * free-space gaps between them.  Each visible row is one item.
 */
typedef struct {
    int          is_part;    /* 1 = partition, 0 = free space  */
    int          pidx;       /* MBR slot 0..3 (partition rows)  */
    unsigned int start;
    unsigned int sectors;
} item_t;

static item_t g_items[NUM_PARTS * 2 + 1];
static int    g_nitems;

static void build_items(void)
{
    g_nitems = 0;

    /* Gather populated partitions, sorted by start LBA (insertion sort). */
    int order[NUM_PARTS], np = 0;
    for (int i = 0; i < NUM_PARTS; i++)
        if (le32(entry(i) + 12) != 0) order[np++] = i;
    for (int a = 1; a < np; a++) {
        int key = order[a], b = a - 1;
        while (b >= 0 && le32(entry(order[b]) + 8) > le32(entry(key) + 8)) {
            order[b + 1] = order[b]; b--;
        }
        order[b + 1] = key;
    }

    unsigned int cursor = ALIGN_SECT;   /* free space starts past embed gap */
    for (int k = 0; k < np; k++) {
        int i = order[k];
        unsigned int st = le32(entry(i) + 8);
        unsigned int ct = le32(entry(i) + 12);
        if (st > cursor && st - cursor >= ALIGN_SECT) {
            g_items[g_nitems].is_part = 0;
            g_items[g_nitems].start   = cursor;
            g_items[g_nitems].sectors = st - cursor;
            g_nitems++;
        }
        g_items[g_nitems].is_part = 1;
        g_items[g_nitems].pidx    = i;
        g_items[g_nitems].start   = st;
        g_items[g_nitems].sectors = ct;
        g_nitems++;
        unsigned int end = st + ct;
        if (end > cursor) cursor = end;
    }
    /* Trailing free space. */
    if (g_total > cursor && g_total - cursor >= ALIGN_SECT) {
        g_items[g_nitems].is_part = 0;
        g_items[g_nitems].start   = cursor;
        g_items[g_nitems].sectors = g_total - cursor;
        g_nitems++;
    }
    if (g_nitems == 0) {   /* empty disk: one big free row */
        g_items[0].is_part = 0;
        g_items[0].start   = ALIGN_SECT;
        g_items[0].sectors = (g_total > ALIGN_SECT) ? g_total - ALIGN_SECT : 0;
        g_nitems = 1;
    }
}

static int first_free_slot(void)
{
    for (int i = 0; i < NUM_PARTS; i++)
        if (le32(entry(i) + 12) == 0) return i;
    return -1;
}

/* ---- action bar --------------------------------------------------------- */

static const char *g_btns[] = { "Bootable", "Delete", "New", "Type", "Write", "Quit" };
#define NBTN ((int)(sizeof(g_btns) / sizeof(g_btns[0])))

#define TABLE_TOP   7

static int g_sel;       /* selected item row */
static int g_btn;       /* selected action button */

static void draw(void)
{
    fill_bg();

    /* Title. */
    const char *title = "cfdisk (Makar)";
    put_str((g_cols - (int)ustrlen(title)) / 2, 0, title, CLR_TITLE);

    /* Disk summary. */
    char buf[64], num[12];
    ucopy(buf, "Disk Drive: ");
    ucopy(buf + ustrlen(buf), g_dev);
    put_str(2, 2, buf, CLR_ROW);

    int n = 0;
    ucopy(buf, "Size: "); n = (int)ustrlen(buf);
    n += utoa(g_total, num); ucopy(buf + ustrlen(buf), num);
    ucopy(buf + ustrlen(buf), " sectors (");
    utoa(g_total / SECT_PER_MIB, num); ucopy(buf + ustrlen(buf), num);
    ucopy(buf + ustrlen(buf), " MiB)");
    put_str(2, 3, buf, CLR_ROW);

    /* Column header. */
    put_str(2,  5, "Name",   CLR_HEAD);
    put_str(12, 5, "Flags",  CLR_HEAD);
    put_str(22, 5, "Type",   CLR_HEAD);
    put_str(38, 5, "Start",  CLR_HEAD);
    put_str(50, 5, "Sectors",CLR_HEAD);
    put_str(64, 5, "Size(MiB)", CLR_HEAD);
    for (int c = 2; c < g_cols - 2; c++) put_at(c, 6, '-', CLR_ROW);

    /* Rows. */
    for (int i = 0; i < g_nitems; i++) {
        int row = TABLE_TOP + i;
        unsigned char clr = (i == g_sel) ? CLR_ROWSEL : CLR_ROW;
        /* Paint full-width selection band. */
        for (int c = 2; c < g_cols - 2; c++) put_at(c, row, ' ', clr);

        item_t *it = &g_items[i];
        char name[8] = "        ", flags[6] = "", num2[12];
        if (it->is_part) {
            name[0] = 'h'; name[1] = 'd'; name[2] = 'a';
            name[3] = (char)('1' + it->pidx); name[4] = '\0';
            if (entry(it->pidx)[0] == 0x80) ucopy(flags, "Boot");
        } else {
            ucopy(name, "");
        }
        put_field(2, row, name, 9, clr);
        put_field(12, row, flags, 9, clr);
        if (it->is_part)
            put_field(22, row, type_name(entry(it->pidx)[4]), 15, clr);
        else
            put_field(22, row, "Free Space", 15, clr);
        utoa(it->start, num2);   put_field(38, row, num2, 11, clr);
        utoa(it->sectors, num2); put_field(50, row, num2, 13, clr);
        utoa(it->sectors / SECT_PER_MIB, num2); put_field(64, row, num2, 11, clr);
    }

    /* Action bar. */
    int bar_row = g_rows - 3;
    int col = 4;
    for (int b = 0; b < NBTN; b++) {
        unsigned char clr = (b == g_btn) ? CLR_BTNSEL : CLR_BTN;
        put_at(col++, bar_row, '[', clr);
        put_at(col++, bar_row, ' ', clr);
        put_str(col, bar_row, g_btns[b], clr); col += (int)ustrlen(g_btns[b]);
        put_at(col++, bar_row, ' ', clr);
        put_at(col++, bar_row, ']', clr);
        col += 2;
    }

    /* Help / status. */
    put_str(2, g_rows - 1,
            g_dirty ? "Up/Down: row  Left/Right: action  Enter: do  (unwritten changes)"
                    : "Up/Down: row  Left/Right: action  Enter: do  q: quit",
            CLR_STATUS);

    flush();
    sys_set_cursor((unsigned int)g_cols - 1, (unsigned int)g_rows - 1);
}

/* ---- status-row prompt -------------------------------------------------- */
/*
 * Paint <label> on the status row and read a line via sys_getkey.  Returns
 * 1 on Enter (text in out), 0 on ESC/Ctrl-C cancel.
 */
static int prompt(const char *label, char *out, int maxlen)
{
    int row = g_rows - 1, len = 0;
    out[0] = '\0';
    for (;;) {
        /* repaint prompt line */
        for (int c = 0; c < g_cols; c++) put_at(c, row, ' ', CLR_WARN);
        put_str(2, row, label, CLR_WARN);
        int base = 2 + (int)ustrlen(label) + 1;
        put_str(base, row, out, CLR_WARN);
        flush();
        sys_set_cursor((unsigned int)(base + len), (unsigned int)row);

        int c = sys_getkey();
        if (c == '\n' || c == '\r') return 1;
        if (c == 0x1B || c == KEY_CTRL_C) return 0;
        if (c == '\b') { if (len > 0) out[--len] = '\0'; continue; }
        if (c >= 0x20 && c < 0x7F && len < maxlen - 1) {
            out[len++] = (char)c; out[len] = '\0';
        }
    }
}

/* Transient one-line message (warn colour); returns after any key. */
static void message(const char *msg)
{
    int row = g_rows - 1;
    for (int c = 0; c < g_cols; c++) put_at(c, row, ' ', CLR_WARN);
    put_str(2, row, msg, CLR_WARN);
    put_str(2 + (int)ustrlen(msg) + 2, row, "[press a key]", CLR_WARN);
    flush();
    sys_getkey();
}

/*
 * size token -> sector count, against a free region [start, avail).
 *   max / M / G / N% / bare sectors.  Clamped to avail.
 */
static unsigned int parse_size(const char *s, unsigned int avail)
{
    while (*s == ' ') s++;
    if ((s[0] == 'm' || s[0] == 'M') && (s[1] == 'a' || s[1] == 'A') &&
        (s[2] == 'x' || s[2] == 'X'))
        return avail;

    unsigned int v = 0; int got = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10u + (unsigned int)(*s++ - '0'); got = 1; }
    if (!got) return 0;
    while (*s == ' ') s++;

    unsigned int out;
    char u = *s;
    if (u == '%') {
        if (v >= 100u) out = avail;
        else out = (avail / 100u) * v + ((avail % 100u) * v) / 100u;
    } else if (u == 'g' || u == 'G') {
        out = v * SECT_PER_MIB * 1024u;
    } else if (u == 'm' || u == 'M') {
        out = v * SECT_PER_MIB;
    } else {
        out = v;   /* bare sectors */
    }
    if (out > avail) out = avail;
    return out;
}

/* ---- actions ------------------------------------------------------------ */

static void act_new(void)
{
    item_t *it = &g_items[g_sel];
    if (it->is_part) { message("Select a Free Space row to create a partition."); return; }
    int slot = first_free_slot();
    if (slot < 0) { message("All 4 primary slots are used."); return; }

    /* Align start up to 1 MiB inside the free region. */
    unsigned int start = (it->start + (ALIGN_SECT - 1)) & ~(ALIGN_SECT - 1);
    unsigned int region_end = it->start + it->sectors;
    if (start >= region_end) { message("Free region too small after alignment."); return; }
    unsigned int avail = region_end - start;

    char in[24], lbl[40] = "Size (max, NM, NG, N%, sectors): ";
    if (!prompt(lbl, in, sizeof(in))) return;
    unsigned int count = parse_size(in, avail);
    if (count == 0) { message("Invalid or zero size."); return; }

    unsigned char *e = entry(slot);
    for (int i = 0; i < PART_ENTRY; i++) e[i] = 0;
    e[4] = 0x0C;                       /* FAT32 LBA default */
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    wr32(e + 8, start);
    wr32(e + 12, count);
    g_dirty = 1;
}

static void act_delete(void)
{
    item_t *it = &g_items[g_sel];
    if (!it->is_part) { message("Not a partition."); return; }
    unsigned char *e = entry(it->pidx);
    for (int i = 0; i < PART_ENTRY; i++) e[i] = 0;
    g_dirty = 1;
}

static void act_bootable(void)
{
    item_t *it = &g_items[g_sel];
    if (!it->is_part) { message("Not a partition."); return; }
    unsigned char *e = entry(it->pidx);
    e[0] = (e[0] == 0x80) ? 0x00 : 0x80;
    g_dirty = 1;
}

/* Map a friendly name (or hex) to an MBR type byte; 0 = unrecognised. */
static unsigned char type_from_token(const char *s)
{
    /* tiny case-insensitive compare against known names */
    struct { const char *n; unsigned char t; } tbl[] = {
        { "fat32", 0x0C }, { "fat16", 0x06 }, { "fat12", 0x01 },
        { "ext2",  0x83 }, { "ext3",  0x83 }, { "ext4",  0x83 },
        { "linux", 0x83 }, { "swap",  0x82 }, { "ntfs",  0x07 },
    };
    for (int i = 0; i < (int)(sizeof(tbl) / sizeof(tbl[0])); i++) {
        const char *a = s, *b = tbl[i].n; int eq = 1;
        while (*a || *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
            if (ca != cb) { eq = 0; break; }
            a++; b++;
        }
        if (eq) return tbl[i].t;
    }
    return (unsigned char)parse_hex(s);    /* fall back to hex */
}

static void act_type(void)
{
    item_t *it = &g_items[g_sel];
    if (!it->is_part) { message("Not a partition."); return; }
    char in[16];
    if (!prompt("Type (fat32 ext2 swap ntfs, or hex e.g. 0c 83): ", in, sizeof(in)))
        return;
    unsigned char t = type_from_token(in);
    if (t == 0) { message("Unrecognised type."); return; }
    entry(it->pidx)[4] = t;
    g_dirty = 1;
}

static int act_write(void)   /* returns 1 if a write happened */
{
    char in[8];
    if (!prompt("Write partition table to disk? (type 'yes'): ", in, sizeof(in))) return 0;
    if (!(in[0] == 'y' && in[1] == 'e' && in[2] == 's' && in[3] == '\0')) {
        message("Not written.");
        return 0;
    }
    mbr[510] = 0x55; mbr[511] = 0xAA;
    if (sys_lseek(g_fd, 0, SEEK_SET) != 0) { message("seek failed"); return 0; }
    long w = sys_write(g_fd, mbr, SECTOR_SIZE);
    if (w != (long)SECTOR_SIZE) { message("write failed"); return 0; }
    g_dirty = 0;
    message("Partition table written.  Re-mount to pick up changes.");
    return 1;
}

static void do_action(void)
{
    switch (g_btn) {
    case 0: act_bootable(); break;
    case 1: act_delete();   break;
    case 2: act_new();      break;
    case 3: act_type();     break;
    case 4: act_write();    break;
    default: break;         /* Quit handled in main loop */
    }
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc > 1) g_dev = argv[1];

    g_fd = sys_open(g_dev, O_RDWR);
    if (g_fd < 0) {
        const char *m = "cfdisk: cannot open device\n";
        sys_write(2, m, ustrlen(m));
        sys_exit(1);
    }
    long r = sys_read(g_fd, mbr, SECTOR_SIZE);
    if (r != (long)SECTOR_SIZE) {
        /* No readable MBR: treat as a blank table on a zero-length read. */
        for (int i = 0; i < (int)SECTOR_SIZE; i++) mbr[i] = 0;
    }
    /* If the signature is absent, start from a blank table. */
    if (!(mbr[510] == 0x55 && mbr[511] == 0xAA)) {
        for (int i = PART_OFFSET; i < (int)SECTOR_SIZE; i++) mbr[i] = 0;
    }

    long dev_bytes = sys_lseek(g_fd, 0, SEEK_END);
    g_total = (dev_bytes > 0) ? (unsigned int)((unsigned long)dev_bytes / SECTOR_SIZE) : 0u;
    sys_lseek(g_fd, 0, SEEK_SET);

    long ts = sys_term_size();
    g_cols = (int)((ts >> 16) & 0xFFFF);
    g_rows = (int)(ts & 0xFFFF);
    if (g_cols <= 0 || g_cols > MAX_COLS) g_cols = 80;
    if (g_rows <= 0 || g_rows > MAX_ROWS) g_rows = 25;

    sys_tty_clear(CLR_BG);

    for (;;) {
        build_items();
        if (g_sel >= g_nitems) g_sel = g_nitems - 1;
        if (g_sel < 0) g_sel = 0;
        draw();

        int c = sys_getkey();
        if (c == KEY_ARROW_UP)    { if (g_sel > 0) g_sel--; }
        else if (c == KEY_ARROW_DOWN)  { if (g_sel < g_nitems - 1) g_sel++; }
        else if (c == KEY_ARROW_LEFT)  { if (g_btn > 0) g_btn--; }
        else if (c == KEY_ARROW_RIGHT) { if (g_btn < NBTN - 1) g_btn++; }
        else if (c == '\n' || c == '\r') {
            if (g_btn == NBTN - 1) break;   /* Quit */
            do_action();
        }
        /* Letter shortcuts (cfdisk-style). */
        else if (c == 'b' || c == 'B') act_bootable();
        else if (c == 'd' || c == 'D') act_delete();
        else if (c == 'n' || c == 'N') act_new();
        else if (c == 't' || c == 'T') act_type();
        else if (c == 'w' || c == 'W') act_write();
        else if (c == 'q' || c == 'Q') break;
    }

    sys_close(g_fd);
    sys_shell_clear();
    sys_exit(0);
    return 0;
}
