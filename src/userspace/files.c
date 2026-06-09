/* files.elf -- minimal fullscreen file browser (ring-3, freestanding).
 *
 * A Midnight-Commander-lite list pane.  Arrow up/down to move, Enter to
 * descend into a directory, Backspace for the parent, q/Esc to quit.  Launched
 * fullscreen from the shell or the gui "Files" icon (execve).  PoC: list +
 * navigate only (no copy/delete/open-file yet).
 */
#include "syscall.h"

#define MAXN 256
#define NAMW 64
static char          names[MAXN][NAMW];
static unsigned char types[MAXN];
static char          cwd[256];

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void cpy(char *d, const char *s, int max)
{ int i = 0; while (s[i] && i < max - 1) { d[i] = s[i]; i++; } d[i] = 0; }

static int load(void)
{
    struct dirent de; int n = 0;
    for (unsigned i = 0; n < MAXN; i++) {
        if (sys_readdir(".", i, &de) != 1) break;
        cpy(names[n], de.d_name, NAMW);
        types[n] = de.d_type;
        n++;
    }
    sys_getcwd(cwd, sizeof cwd);
    return n;
}

static void put_at(unsigned row, unsigned col, const char *s)
{
    sys_set_cursor(col, row);
    sys_write(1, s, slen(s));
}

static void draw(int n, int sel, int off, unsigned rows)
{
    sys_shell_clear();
    put_at(0, 0, "Files  ");
    put_at(0, 7, cwd);
    put_at(1, 0, "up/down move  enter open dir  bksp parent  q quit");
    unsigned vis = rows > 4 ? rows - 4 : 1;
    for (unsigned r = 0; r < vis; r++) {
        int idx = off + (int)r;
        if (idx >= n) break;
        char line[80]; int p = 0;
        line[p++] = (idx == sel) ? '>' : ' ';
        line[p++] = ' ';
        const char *nm = names[idx];
        for (int j = 0; nm[j] && p < 76; j++) line[p++] = nm[j];
        if (types[idx] == DT_DIR && p < 78) line[p++] = '/';
        line[p] = 0;
        put_at(3 + r, 0, line);
    }
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    sys_keyboard_raw(1);
    unsigned rows = sys_term_rows(); if (!rows) rows = 25;
    int sel = 0, off = 0, n = load();

    for (;;) {
        if (sel < 0) sel = 0;
        if (sel >= n) sel = n ? n - 1 : 0;
        unsigned vis = rows > 4 ? rows - 4 : 1;
        if (sel < off) off = sel;
        if (sel >= off + (int)vis) off = sel - (int)vis + 1;
        draw(n, sel, off, rows);

        unsigned char b = (unsigned char)sys_getkey();
        if (b == 'q' || b == 'Q' || b == 27) break;
        else if (b == KEY_ARROW_UP)   sel--;
        else if (b == KEY_ARROW_DOWN) sel++;
        else if (b == '\n' || b == '\r' || b == 13) {
            if (n && types[sel] == DT_DIR) { sys_chdir(names[sel]); sel = off = 0; n = load(); }
        } else if (b == 8 || b == 127) { sys_chdir(".."); sel = off = 0; n = load(); }
    }

    sys_keyboard_raw(0);
    sys_shell_clear();
    return 0;
}
