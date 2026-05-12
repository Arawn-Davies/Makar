/*
 * kbtester.c -- visual press-all-keys keyboard tester (ring-3).
 *
 * Renders a QWERTY keyboard layout on the screen. Each key is drawn as a
 * cell on the VGA grid; pressing a key highlights it (sticky -- once
 * pressed, stays green until exit). Modifier presses are inferred from
 * the byte stream:
 *
 *   Shift  -- highlighted whenever an uppercase letter or shifted symbol
 *             arrives (we never get a byte for Shift directly).
 *   Ctrl   -- highlighted whenever a Ctrl+letter control code arrives.
 *
 * Caveats baked into the current driver:
 *
 *   - F1..F12 alone, Alt, Caps Lock, Win/Menu, Insert/Delete/Home/End/
 *     PgUp/PgDn, PrintScreen, Scroll Lock, Pause, and most Numpad keys
 *     produce no cooked byte today, so they're rendered as greyed-out
 *     "untestable" boxes. They light up if/when translate_make() in the
 *     kernel grows the corresponding sentinels.
 *
 * Three side-channels are populated for off-host verification:
 *
 *   1. The screen   - operator can watch the lights.
 *   2. COM1 serial  - every accepted byte is logged as a one-line record
 *                     ("[NNNN] 0xHH dec=DDD name=...") via SYS_WRITE_SERIAL,
 *                     so a serial-stdio recorder can verify the byte
 *                     stream without scraping the framebuffer.
 *   3. Input echo   - the last 60 printable characters are mirrored at
 *                     the top of the screen as a typing buffer.
 *
 * Exit: Ctrl+C (sentinel 0x03). Pressing 'q' on its own does NOT exit
 *       in this version because the operator legitimately needs to test
 *       'q' and 'Q'.
 *
 * Falls back to a one-line-per-key text mode if the terminal is narrower
 * than 70 columns (e.g. VESA TTY at font_scale=2 / 720p / 40 cols).
 */

#include "syscall.h"

/* ---------------------------------------------------------------------------
 * Minimal libc helpers.  No memcpy/memset/printf available in this env --
 * we hand-roll the small subset we need.
 * ------------------------------------------------------------------------ */

static unsigned int u_strlen(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

static void put_str(const char *s)
{
    sys_write(1, s, u_strlen(s));
}

static void put_serial(const char *s, unsigned int n)
{
    sys_write_serial(s, n);
}

static void fmt_hex2(char *buf, unsigned int n)
{
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = hex[(n >> 4) & 0xF];
    buf[1] = hex[n & 0xF];
}

static void fmt_dec3(char *buf, unsigned int n)
{
    buf[0] = (char)('0' + (n / 100) % 10);
    buf[1] = (char)('0' + (n / 10) % 10);
    buf[2] = (char)('0' + n % 10);
}

static void fmt_dec4(char *buf, unsigned int n)
{
    buf[0] = (char)('0' + (n / 1000) % 10);
    buf[1] = (char)('0' + (n / 100) % 10);
    buf[2] = (char)('0' + (n / 10) % 10);
    buf[3] = (char)('0' + n % 10);
}

/* ---------------------------------------------------------------------------
 * Byte -> human-readable name (used in the status line and serial log).
 * ------------------------------------------------------------------------ */

static const char *byte_name(unsigned char b)
{
    switch (b) {
        case 0x80: return "ARROW_UP";
        case 0x81: return "ARROW_DOWN";
        case 0x82: return "ARROW_LEFT";
        case 0x83: return "ARROW_RIGHT";
        case 0x84: return "F1";
        case 0x85: return "F2";
        case 0x86: return "F3";
        case 0x87: return "F4";
        case 0x88: return "FOCUS_GAIN";
        case 0x89: return "F5";
        case 0x8A: return "F6";
        case 0x8B: return "F7";
        case 0x8C: return "F8";
        case 0x8D: return "F9";
        case 0x8E: return "F10";
        case 0x8F: return "F11";
        case 0x90: return "F12";
        case 0x91: return "Shift";
        case 0x92: return "Ctrl";
        case 0x93: return "Alt";
        case 0x94: return "CapsLock";
        case 0x95: return "Super";
        case 0x96: return "Menu";
        case 0x00: return "NUL";
        case 0x03: return "Ctrl-C";
        case 0x08: return "Backspace";
        case 0x09: return "Tab";
        case 0x0A: return "Enter";
        case 0x0D: return "CR";
        case 0x1B: return "Escape";
        case 0x20: return "Space";
        case 0x7F: return "Delete";
    }
    if (b >= 0x01 && b <= 0x1A) return "Ctrl+letter";
    if (b >= 0x21 && b <= 0x7E) return "printable";
    return "extended";
}

/* ===========================================================================
 * VGA colour helpers
 *
 * Only the key icons get a custom palette. All other text (title, input
 * field, "Last:" status, footer hint) is rendered via sys_write so it
 * picks up the shell's current t_color naturally -- no hardcoded fg/bg
 * for chrome, so user-set fgcol/bgcol settings are respected.
 * ======================================================================== */

#define CLR_DEFAULT     VGA_CLR(VGA_LGREY, VGA_BLACK)    /* normal key      */
#define CLR_PRESSED     VGA_CLR(VGA_WHITE, VGA_GREEN)    /* sticky-pressed  */
#define CLR_UNTEST      VGA_CLR(VGA_DGREY, VGA_BLACK)    /* untestable      */

/* ===========================================================================
 * Cell-buffer flush helper -- batches sys_putch_at calls.
 * ======================================================================== */

#define MAX_BATCH 256
static tty_cell_t batch[MAX_BATCH];
static unsigned int batch_n = 0;

static void flush_batch(void)
{
    if (batch_n) {
        sys_putch_at(batch, batch_n);
        batch_n = 0;
    }
}

static void emit(unsigned char col, unsigned char row, unsigned char ch, unsigned char clr)
{
    if (batch_n == MAX_BATCH) flush_batch();
    batch[batch_n].col = col;
    batch[batch_n].row = row;
    batch[batch_n].ch  = ch;
    batch[batch_n].clr = clr;
    batch_n++;
}

/*
 * Write a string at (col, row) using the shell's current t_color (via
 * sys_write to fd 1). Used for chrome -- title, labels, input echo,
 * status, footer -- so user-set fgcol/bgcol settings are respected.
 */
static void put_at(unsigned int col, unsigned int row, const char *s)
{
    sys_set_cursor(col, row);
    sys_write(1, s, u_strlen(s));
}

/*
 * Fill `n` cells from (col, row) rightwards with spaces in the shell's
 * current t_color. Used to clear chrome rows / wipe a line before
 * redrawing input echo or status.
 */
static void fill_spaces(unsigned int col, unsigned int row, unsigned int n)
{
    char buf[80];
    if (n > sizeof(buf)) n = sizeof(buf);
    for (unsigned int i = 0; i < n; i++) buf[i] = ' ';
    sys_set_cursor(col, row);
    sys_write(1, buf, n);
}

/*
 * Wipe the entire screen using the shell's current fg/bg (via fd 1 so we
 * don't need to know the palette ourselves).  Used at launch and at exit
 * so kbtester leaves a clean canvas in both directions.
 *
 * Resolution-agnostic: fill_spaces' internal buffer caps a single write at
 * 80 cells, so for wider terminals we chunk across the row.  No upper
 * bound on rows — sys_term_rows() drives the outer loop.
 */
static void clear_screen(unsigned int cols, unsigned int rows)
{
    for (unsigned int r = 0; r < rows; r++) {
        unsigned int x = 0;
        while (x < cols) {
            unsigned int chunk = cols - x;
            if (chunk > 80) chunk = 80;
            fill_spaces(x, r, chunk);
            x += chunk;
        }
    }
    sys_set_cursor(0, 0);
}

/* ===========================================================================
 * Key descriptor table.
 *
 * Each entry describes one drawable key, its position on the grid, and how
 * to detect that it was pressed:
 *
 *   match_a   - primary cooked byte (0 if no direct match)
 *   match_b   - alternate cooked byte (e.g. uppercase / shifted)
 *   special   - SP_NONE for normal keys; SP_SHIFT/SP_CTRL for inferred
 *               modifier highlighting; SP_UNTEST for greyed-out keys.
 * ======================================================================== */

enum {
    SP_NONE   = 0,
    SP_SHIFT  = 1,
    SP_CTRL   = 2,
    SP_UNTEST = 3,
};

typedef struct {
    const char   *label;
    unsigned char match_a;
    unsigned char match_b;
    unsigned char col;
    unsigned char row;
    unsigned char w;
    unsigned char special;
    unsigned char pressed;
} key_t;

/* Layout designed for an 80-column terminal. Rows start at KB_ROW0. */
#define KB_ROW0   7

#define KN(L, A, B, C, R, W)  { L, A, B, C, R, W, SP_NONE,   0 }
#define KU(L, C, R, W)        { L, 0, 0, C, R, W, SP_UNTEST, 0 }
#define KS(L, C, R, W)        { L, 0, 0, C, R, W, SP_SHIFT,  0 }
#define KC(L, C, R, W)        { L, 0, 0, C, R, W, SP_CTRL,   0 }

static key_t KEYS[] = {
    /* --- function row (row 0) ------------------------------------------- */
    KN("Esc", 0x1B, 0,                              2, KB_ROW0 + 0, 5),
    KN("F1",  0x84, 0,                             11, KB_ROW0 + 0, 4),
    KN("F2",  0x85, 0,                             16, KB_ROW0 + 0, 4),
    KN("F3",  0x86, 0,                             21, KB_ROW0 + 0, 4),
    KN("F4",  0x87, 0,                             26, KB_ROW0 + 0, 4),
    KN("F5",  0x89, 0,                             32, KB_ROW0 + 0, 4),
    KN("F6",  0x8A, 0,                             37, KB_ROW0 + 0, 4),
    KN("F7",  0x8B, 0,                             42, KB_ROW0 + 0, 4),
    KN("F8",  0x8C, 0,                             47, KB_ROW0 + 0, 4),
    KN("F9",  0x8D, 0,                             53, KB_ROW0 + 0, 4),
    /* F10/F11/F12 need an extra inner cell so the 3-char label fits;
     * width 4 only leaves room for two characters between the brackets,
     * which clipped every two-digit F-key to "F1". */
    KN("F10", 0x8E, 0,                             58, KB_ROW0 + 0, 5),
    KN("F11", 0x8F, 0,                             64, KB_ROW0 + 0, 5),
    KN("F12", 0x90, 0,                             70, KB_ROW0 + 0, 5),

    /* --- number row (row 2) --------------------------------------------- */
    KN("`",   '`', '~',                             2, KB_ROW0 + 2, 3),
    KN("1",   '1', '!',                             6, KB_ROW0 + 2, 3),
    KN("2",   '2', '@',                            10, KB_ROW0 + 2, 3),
    KN("3",   '3', '#',                            14, KB_ROW0 + 2, 3),
    KN("4",   '4', '$',                            18, KB_ROW0 + 2, 3),
    KN("5",   '5', '%',                            22, KB_ROW0 + 2, 3),
    KN("6",   '6', '^',                            26, KB_ROW0 + 2, 3),
    KN("7",   '7', '&',                            30, KB_ROW0 + 2, 3),
    KN("8",   '8', '*',                            34, KB_ROW0 + 2, 3),
    KN("9",   '9', '(',                            38, KB_ROW0 + 2, 3),
    KN("0",   '0', ')',                            42, KB_ROW0 + 2, 3),
    KN("-",   '-', '_',                            46, KB_ROW0 + 2, 3),
    KN("=",   '=', '+',                            50, KB_ROW0 + 2, 3),
    KN("Bksp",0x08, 0,                             54, KB_ROW0 + 2, 6),

    /* --- QWERTY row (row 3) --------------------------------------------- */
    KN("Tab", 0x09, 0,                              2, KB_ROW0 + 3, 5),
    KN("Q",   'q', 'Q',                             8, KB_ROW0 + 3, 3),
    KN("W",   'w', 'W',                            12, KB_ROW0 + 3, 3),
    KN("E",   'e', 'E',                            16, KB_ROW0 + 3, 3),
    KN("R",   'r', 'R',                            20, KB_ROW0 + 3, 3),
    KN("T",   't', 'T',                            24, KB_ROW0 + 3, 3),
    KN("Y",   'y', 'Y',                            28, KB_ROW0 + 3, 3),
    KN("U",   'u', 'U',                            32, KB_ROW0 + 3, 3),
    KN("I",   'i', 'I',                            36, KB_ROW0 + 3, 3),
    KN("O",   'o', 'O',                            40, KB_ROW0 + 3, 3),
    KN("P",   'p', 'P',                            44, KB_ROW0 + 3, 3),
    KN("[",   '[', '{',                            48, KB_ROW0 + 3, 3),
    KN("]",   ']', '}',                            52, KB_ROW0 + 3, 3),
    KN("\\",  '\\','|',                            56, KB_ROW0 + 3, 3),

    /* --- ASDF row (row 4) ----------------------------------------------- */
    KN("Caps",0x94, 0,                              2, KB_ROW0 + 4, 6),
    KN("A",   'a', 'A',                             9, KB_ROW0 + 4, 3),
    KN("S",   's', 'S',                            13, KB_ROW0 + 4, 3),
    KN("D",   'd', 'D',                            17, KB_ROW0 + 4, 3),
    KN("F",   'f', 'F',                            21, KB_ROW0 + 4, 3),
    KN("G",   'g', 'G',                            25, KB_ROW0 + 4, 3),
    KN("H",   'h', 'H',                            29, KB_ROW0 + 4, 3),
    KN("J",   'j', 'J',                            33, KB_ROW0 + 4, 3),
    KN("K",   'k', 'K',                            37, KB_ROW0 + 4, 3),
    KN("L",   'l', 'L',                            41, KB_ROW0 + 4, 3),
    KN(";",   ';', ':',                            45, KB_ROW0 + 4, 3),
    KN("'",   '\'', '"',                           49, KB_ROW0 + 4, 3),
    KN("Enter",0x0A, 0,                            53, KB_ROW0 + 4, 7),

    /* --- ZXCV row (row 5) ----------------------------------------------- */
    KS("Shift",                                     2, KB_ROW0 + 5, 7),
    KN("Z",   'z', 'Z',                            10, KB_ROW0 + 5, 3),
    KN("X",   'x', 'X',                            14, KB_ROW0 + 5, 3),
    KN("C",   'c', 'C',                            18, KB_ROW0 + 5, 3),
    KN("V",   'v', 'V',                            22, KB_ROW0 + 5, 3),
    KN("B",   'b', 'B',                            26, KB_ROW0 + 5, 3),
    KN("N",   'n', 'N',                            30, KB_ROW0 + 5, 3),
    KN("M",   'm', 'M',                            34, KB_ROW0 + 5, 3),
    KN(",",   ',', '<',                            38, KB_ROW0 + 5, 3),
    KN(".",   '.', '>',                            42, KB_ROW0 + 5, 3),
    KN("/",   '/', '?',                            46, KB_ROW0 + 5, 3),
    KS("Shift",                                    50, KB_ROW0 + 5, 7),

    /* --- bottom row (row 6) ---------------------------------------------
     * In raw mode the kernel routes a sentinel byte for every Ctrl/Alt/
     * Super/Caps press, so the cells are SP_NONE with explicit match
     * bytes rather than SP_CTRL inference.  The SP_CTRL fallback is
     * still useful for cooked mode (Ctrl+letter inference) but raw mode
     * gives us the press event directly. */
    KN("Ctrl",0x92, 0,                              2, KB_ROW0 + 6, 5),
    KN("Win", 0x95, 0,                              8, KB_ROW0 + 6, 4),
    KN("Alt", 0x93, 0,                             13, KB_ROW0 + 6, 4),
    KN("Space", ' ', 0,                            18, KB_ROW0 + 6,15),
    KN("Alt", 0x93, 0,                             34, KB_ROW0 + 6, 4),
    KN("Win", 0x95, 0,                             39, KB_ROW0 + 6, 4),
    KN("Ctrl",0x92, 0,                             44, KB_ROW0 + 6, 5),

    /* --- arrows (rows 8-9) ---------------------------------------------- */
    KN("^",   0x80, 0,                             32, KB_ROW0 + 8, 3),  /* up    */
    KN("<",   0x82, 0,                             28, KB_ROW0 + 9, 3),  /* left  */
    KN("v",   0x81, 0,                             32, KB_ROW0 + 9, 3),  /* down  */
    KN(">",   0x83, 0,                             36, KB_ROW0 + 9, 3),  /* right */
};

#define NUM_KEYS  (sizeof(KEYS)/sizeof(KEYS[0]))

/* ===========================================================================
 * Drawing
 * ======================================================================== */

/*
 * draw_key - render one key cell using its current pressed state.
 *
 * Layout of a key of width w:
 *
 *     [ label ]
 *
 * Brackets and inner area share the key's status colour (default,
 * pressed, or untestable). If the label is shorter than the inner
 * area (w-2) we centre it; if longer, it's clipped.
 */
static void draw_key(const key_t *k)
{
    unsigned char clr;
    if      (k->special == SP_UNTEST) clr = CLR_UNTEST;
    else if (k->pressed)              clr = CLR_PRESSED;
    else                              clr = CLR_DEFAULT;

    /* whole cell -- brackets and inner -- in the key colour */
    emit(k->col,                  k->row, '[', clr);
    emit((unsigned char)(k->col + k->w - 1), k->row, ']', clr);
    for (unsigned char i = 1; i < (unsigned char)(k->w - 1); i++)
        emit((unsigned char)(k->col + i), k->row, ' ', clr);

    /* centred label */
    unsigned int llen = u_strlen(k->label);
    unsigned int inner = (unsigned int)(k->w - 2);
    unsigned int off = (llen < inner) ? (inner - llen) / 2 : 0;
    for (unsigned int i = 0; i < llen && i < inner; i++)
        emit((unsigned char)(k->col + 1 + off + i), k->row,
             (unsigned char)k->label[i], clr);
}

static void draw_static(unsigned int cols)
{
    /* Title bar (row 0) -- shell colour.  (Full-screen wipe happens in
     * main() before draw_static, so the canvas is already clean.) */
    put_at(0, 0, " kbtester  -- press any key, Ctrl+C to exit");

    /* Separator (row 1) -- shell colour.  Spans the actual terminal
     * width, chunked through an 80-byte buffer to stay resolution
     * agnostic on wide modes. */
    char sep[80];
    for (unsigned int i = 0; i < sizeof(sep); i++) sep[i] = '-';
    unsigned int x = 0;
    while (x < cols) {
        unsigned int chunk = cols - x;
        if (chunk > sizeof(sep)) chunk = sizeof(sep);
        sys_set_cursor(x, 1);
        sys_write(1, sep, chunk);
        x += chunk;
    }

    /* Input + status labels. */
    put_at(0, 2, "Input:");
    put_at(0, 4, "Last :");

    /* Footer hint. */
    put_at(0, KB_ROW0 + 11,
           "Raw mode active: every key reaches kbtester. Ctrl+C exits.");

    /* Key cells -- the only thing with a custom palette. */
    for (unsigned int i = 0; i < NUM_KEYS; i++)
        draw_key(&KEYS[i]);

    flush_batch();
}

/* ===========================================================================
 * Echo + status updates
 * ======================================================================== */

#define ECHO_LEN 60
static char echo_buf[ECHO_LEN + 1];
static unsigned int echo_n = 0;

/*
 * update_echo - append a printable byte to the typing-echo buffer.
 *
 * Behaves like a typing field: backspace deletes the last char, printable
 * bytes append (scrolling via memmove when full), all other bytes are
 * ignored. The visible row is then redrawn from echo_buf.
 */
static void update_echo(unsigned char b)
{
    if (b == 0x08) {                /* backspace */
        if (echo_n) echo_n--;
    } else if (b >= 0x20 && b <= 0x7E) {
        if (echo_n == ECHO_LEN) {
            for (unsigned int i = 0; i < ECHO_LEN - 1; i++)
                echo_buf[i] = echo_buf[i + 1];
            echo_n = ECHO_LEN - 1;
        }
        echo_buf[echo_n++] = (char)b;
    }
    echo_buf[echo_n] = '\0';

    /* Render via shell colour: clear region, then write text + caret. */
    fill_spaces(8, 2, ECHO_LEN + 2);
    sys_set_cursor(8, 2);
    sys_write(1, echo_buf, echo_n);
    sys_write(1, "_", 1);
}

/*
 * update_status - redraw the "Last:" line with hex/dec/name and the count.
 */
static void update_status(unsigned char b, unsigned int count)
{
    char buf[80];
    unsigned int p = 0;
    buf[p++] = ' '; buf[p++] = '0'; buf[p++] = 'x';
    fmt_hex2(&buf[p], b); p += 2;
    buf[p++] = ' '; buf[p++] = ' '; buf[p++] = 'd'; buf[p++] = 'e'; buf[p++] = 'c'; buf[p++] = '=';
    fmt_dec3(&buf[p], b); p += 3;
    buf[p++] = ' '; buf[p++] = ' '; buf[p++] = '(';
    const char *name = byte_name(b);
    unsigned int nlen = u_strlen(name);
    for (unsigned int i = 0; i < nlen && p < sizeof(buf) - 16; i++)
        buf[p++] = name[i];
    buf[p++] = ')';
    buf[p++] = ' '; buf[p++] = ' '; buf[p++] = 'c'; buf[p++] = 'o'; buf[p++] = 'u'; buf[p++] = 'n'; buf[p++] = 't'; buf[p++] = '=';
    fmt_dec4(&buf[p], count); p += 4;
    while (p < 70) buf[p++] = ' ';

    fill_spaces(8, 4, 70);
    sys_set_cursor(8, 4);
    sys_write(1, buf, p);
}

/* ===========================================================================
 * Press detection
 *
 * Walk the key table and mark every key whose match criteria fire on b.
 * - SP_NONE   : matches if b == match_a or b == match_b.
 * - SP_SHIFT  : matches if b is the shifted variant of any printable key
 *               (i.e. b appears as a match_b somewhere in the table).
 * - SP_CTRL   : matches if b is in 0x01..0x1A (Ctrl+letter).
 * - SP_UNTEST : never matches (kept greyed out).
 *
 * Returns the number of keys newly marked pressed (so caller can know
 * whether to redraw modifiers as well as the primary key).
 * ======================================================================== */

static int byte_is_shifted(unsigned char b)
{
    /* Explicit Shift press sentinel (raw mode) lights up the Shift cell
     * directly.  Inferred shift (uppercase letter / shifted symbol) is
     * preserved as a backup for any cooked-mode use. */
    if (b == 0x91) return 1;
    if (b >= 'A' && b <= 'Z') return 1;
    for (unsigned int i = 0; i < NUM_KEYS; i++)
        if (KEYS[i].special == SP_NONE && KEYS[i].match_b && KEYS[i].match_b == b)
            return 1;
    return 0;
}

static void mark_pressed(unsigned char b)
{
    int shifted = byte_is_shifted(b);
    /*
     * "Real" Ctrl+letter codes are 0x01..0x1A, but four bytes in that
     * range come from dedicated keys far more often than from Ctrl:
     *   0x08 Backspace  (also Ctrl-H)
     *   0x09 Tab        (also Ctrl-I)
     *   0x0A LF/Enter   (also Ctrl-J)
     *   0x0D CR         (also Ctrl-M)
     * The cooked driver collapses both sources to the same byte, so we
     * can't tell them apart -- but lighting up Ctrl on every Tab press
     * is wrong far more often than it's right. Trust the dedicated key.
     */
    int is_ctrl = (b >= 0x01 && b <= 0x1A) &&
                  b != 0x08 && b != 0x09 && b != 0x0A && b != 0x0D;

    for (unsigned int i = 0; i < NUM_KEYS; i++) {
        key_t *k = &KEYS[i];
        if (k->special == SP_UNTEST) continue;
        int hit = 0;
        if (k->special == SP_NONE) {
            if (k->match_a && b == k->match_a) hit = 1;
            if (k->match_b && b == k->match_b) hit = 1;
            /* Ctrl+letter uniquely identifies its letter (Ctrl-A == 0x01 etc.) */
            if (is_ctrl) {
                unsigned char letter = (unsigned char)('a' + (b - 1));
                if (k->match_a == letter) hit = 1;
            }
        } else if (k->special == SP_SHIFT && shifted) {
            hit = 1;
        } else if (k->special == SP_CTRL && is_ctrl) {
            hit = 1;
        }
        if (hit && !k->pressed) {
            k->pressed = 1;
            draw_key(k);
        }
    }
}

/* ===========================================================================
 * Serial mirror -- one line per byte for off-host verification
 * ======================================================================== */

static void log_serial(unsigned int count, unsigned char b)
{
    char line[80];
    unsigned int p = 0;

    line[p++] = '['; fmt_dec4(&line[p], count); p += 4; line[p++] = ']';
    line[p++] = ' '; line[p++] = '0'; line[p++] = 'x';
    fmt_hex2(&line[p], b); p += 2;
    line[p++] = ' '; line[p++] = 'd'; line[p++] = 'e'; line[p++] = 'c'; line[p++] = '=';
    fmt_dec3(&line[p], b); p += 3;
    line[p++] = ' '; line[p++] = 'n'; line[p++] = 'a'; line[p++] = 'm'; line[p++] = 'e'; line[p++] = '=';
    const char *name = byte_name(b);
    for (unsigned int i = 0; name[i] && p < sizeof(line) - 2; i++)
        line[p++] = name[i];
    line[p++] = '\n';
    put_serial(line, p);
}

/* ===========================================================================
 * Fallback line-mode for narrow terminals (< 70 cols).
 * ======================================================================== */

static void line_mode_loop(unsigned int cols, unsigned int rows)
{
    clear_screen(cols, rows);
    put_str(
"kbtester (compact mode -- terminal too narrow for visual layout)\n"
"Press any key; Ctrl+C exits. Output mirrored to COM1.\n"
"\n"
    );
    put_serial("KBTESTER_BEGIN compact\n", 23);

    unsigned int count = 0;
    while (1) {
        int k = sys_getkey();
        unsigned char b = (unsigned char)k;
        count++;
        log_serial(count, b);

        char line[64]; unsigned int p = 0;
        line[p++] = '['; fmt_dec4(&line[p], count); p += 4; line[p++] = ']';
        line[p++] = ' '; line[p++] = '0'; line[p++] = 'x';
        fmt_hex2(&line[p], b); p += 2;
        line[p++] = ' ';
        const char *name = byte_name(b);
        for (unsigned int i = 0; name[i] && p < sizeof(line) - 2; i++)
            line[p++] = name[i];
        line[p++] = '\n';
        sys_write(1, line, p);

        if (b == 0x03) {
            clear_screen(cols, rows);
            put_str("KBTESTER_END\n");
            put_serial("KBTESTER_END\n", 13);
            return;
        }
    }
}

/* ===========================================================================
 * main
 * ======================================================================== */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Raw mode: the shell's Alt+Fn TTY switch and Ctrl+A pane prefix are
     * suspended for the duration of this run, modifier and F-key presses
     * arrive as sentinel bytes.  Paired with sys_keyboard_raw(0) on
     * every exit path; shell_exec_elf also resets it after we return as
     * a safety net.  Ctrl+C still fires so the operator can quit. */
    sys_keyboard_raw(1);

    /* Resolution-agnostic: clear the whole screen we were handed, not a
     * hardcoded 80×25 rectangle, so wide VESA modes don't show stale
     * shell text framing kbtester's UI. */
    unsigned int cols = sys_term_cols();
    unsigned int rows = sys_term_rows();
    clear_screen(cols, rows);

    if (cols < 70) {
        line_mode_loop(cols, rows);
        clear_screen(cols, rows);
        sys_keyboard_raw(0);
        return 0;
    }

    draw_static(cols);
    update_echo(0);                /* render empty input field */
    update_status(0, 0);
    flush_batch();

    put_serial("KBTESTER_BEGIN visual\n", 22);

    unsigned int count = 0;
    while (1) {
        int k = sys_getkey();
        unsigned char b = (unsigned char)k;
        count++;

        log_serial(count, b);

        if (b == 0x03) {       /* Ctrl-C: clean exit */
            put_serial("KBTESTER_END (Ctrl+C; see COM1 for full byte log)\n", 50);
            sys_keyboard_raw(0);
            clear_screen(cols, rows);
            /* Leave the cursor at the top with no goodbye text — the shell
             * prompt about to be drawn is the only thing the operator
             * needs to see.  The serial log records the exit. */
            return 0;
        }

        update_echo(b);
        update_status(b, count);
        mark_pressed(b);
        flush_batch();
    }
}
