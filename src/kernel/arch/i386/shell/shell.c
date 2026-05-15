/*
 * shell.c -- kernel REPL core: line input, tokenisation, and command dispatch.
 *
 * Provides:
 *   shell_readline() - echoes input, handles backspace, ends on Enter
 *   shell_run()      - infinite prompt loop (Medli-style UX)
 *
 * Command implementations live in shell_cmds.c and shell_help.c.
 */

#include "shell_priv.h"

#include <kernel/shell.h>
#include <kernel/keyboard.h>
#include <kernel/vtty.h>
#include <kernel/tty.h>
#include <kernel/vga.h>
#include <kernel/vesa.h>
#include <kernel/vesa_tty.h>
#include <kernel/serial.h>
#include <kernel/vfs.h>
#include <kernel/timer.h>
#include <kernel/task.h>
#include <kernel/ktest.h>

#include <string.h>

/* ---------------------------------------------------------------------------
 * Command history ring-buffer (SHELL_HISTORY_SIZE most-recent entries).
 * --------------------------------------------------------------------------- */
#define SHELL_HISTORY_SIZE 16

static char s_history[SHELL_HISTORY_SIZE][SHELL_MAX_INPUT];
static int  s_hist_count = 0;  /* valid entries (capped at SHELL_HISTORY_SIZE) */
static int  s_hist_head  = 0;  /* index of the next write slot                 */

static void history_push(const char *line)
{
    if (!line || !*line)
        return;
    strncpy(s_history[s_hist_head], line, SHELL_MAX_INPUT - 1);
    s_history[s_hist_head][SHELL_MAX_INPUT - 1] = '\0';
    s_hist_head = (s_hist_head + 1) % SHELL_HISTORY_SIZE;
    if (s_hist_count < SHELL_HISTORY_SIZE)
        s_hist_count++;
}

/* ago=0 → most-recent entry; returns NULL when out of range. */
static const char *history_get(int ago)
{
    if (ago < 0 || ago >= s_hist_count)
        return NULL;
    int idx = (s_hist_head - 1 - ago + SHELL_HISTORY_SIZE) % SHELL_HISTORY_SIZE;
    return s_history[idx];
}

/* Forward declaration - defined later in this file. */
static void shell_print_prompt(void);

/* Search path for executables - also used by tab completion to enumerate
 * the available `*.elf` binaries when completing the first token. */
static const char *const s_app_path[] = {
    "/cdrom/apps/",
    "/hd/apps/",
    NULL,
};

/* ---------------------------------------------------------------------------
 * readline_redraw – repaint the input line in-place and position cursors.
 *
 * VGA uses vga_start_col/row (always 80 cols).
 * VESA uses vesa_start_col/row (tty_cols wide, which differs from VGA_WIDTH).
 * Passing separate VESA coords avoids the t_row=0 desync that occurs when
 * VESA is active and the VGA row counter wraps.
 * --------------------------------------------------------------------------- */
static void readline_redraw(const char *buf, size_t len, size_t cur,
                             size_t erase_to,
                             size_t vga_start_col, size_t vga_start_row,
                             uint32_t vesa_start_col, uint32_t vesa_start_row)
{
    uint32_t vcols = vesa_tty_is_ready() ? vesa_tty_get_cols() : VGA_WIDTH;
    size_t render_to = (erase_to > len) ? erase_to : len;

    for (size_t i = 0; i <= render_to; i++) {
        char ch = (i < len) ? buf[i] : ' '; /* spaces erase any previous content */

        size_t vga_lp  = vga_start_col + i;
        size_t vga_row = vga_start_row + vga_lp / VGA_WIDTH;
        size_t vga_col = vga_lp % VGA_WIDTH;
        VGA_MEMORY[vga_row * VGA_WIDTH + vga_col] = make_vgaentry(ch, SHELL_COLOR_VGA);

        if (vesa_tty_is_ready()) {
            uint32_t vlp = vesa_start_col + (uint32_t)i;
            vesa_tty_put_at(ch, vlp % vcols, vesa_start_row + vlp / vcols);
        }
    }

    /* VGA hardware cursor */
    size_t cp = vga_start_col + cur;
    update_cursor(vga_start_row + cp / VGA_WIDTH, cp % VGA_WIDTH);

    /* VESA internal cursor */
    if (vesa_tty_is_ready()) {
        uint32_t vcp = vesa_start_col + (uint32_t)cur;
        vesa_tty_set_cursor(vcp % vcols, vesa_start_row + vcp / vcols);
    }
}

/* ---------------------------------------------------------------------------
 * Tab-completion VFS callback and context
 * --------------------------------------------------------------------------- */

typedef struct {
    const char *pfx;
    size_t      pfxlen;
    int         n;
    char        matches[32][64];
    int         is_dir[32];
} tab_vctx_t;

static void tab_complete_cb(const char *name, int is_dir, void *ctx)
{
    tab_vctx_t *v = (tab_vctx_t *)ctx;
    if (v->n >= 32) return;
    if (strncmp(name, v->pfx, v->pfxlen) != 0) return;
    strncpy(v->matches[v->n], name, 63);
    v->matches[v->n][63] = '\0';
    v->is_dir[v->n] = is_dir;
    v->n++;
}

/* First-token completion variant: only accept regular files ending in
 * ".elf"; record the name with the extension stripped so the suggestion
 * matches what the user actually types to run it. */
static void tab_app_cb(const char *name, int is_dir, void *ctx)
{
    if (is_dir) return;
    tab_vctx_t *v = (tab_vctx_t *)ctx;
    if (v->n >= 32) return;
    size_t nlen = strlen(name);
    if (nlen < 4 || strcmp(name + nlen - 4, ".elf") != 0) return;
    size_t base_len = nlen - 4;
    if (base_len < v->pfxlen) return;
    if (strncmp(name, v->pfx, v->pfxlen) != 0) return;
    /* Avoid duplicates if the same name exists on multiple app paths. */
    for (int i = 0; i < v->n; i++) {
        if (strncmp(v->matches[i], name, base_len) == 0 &&
            v->matches[i][base_len] == '\0')
            return;
    }
    if (base_len > 63) base_len = 63;
    memcpy(v->matches[v->n], name, base_len);
    v->matches[v->n][base_len] = '\0';
    v->is_dir[v->n] = 0;
    v->n++;
}

/* ---------------------------------------------------------------------------
 * shell_readline – read a line from the PS/2 keyboard into buf.
 *
 * Handles:
 *   Printable       – insert at cursor (inline editing).
 *   '\b'            – delete char before cursor.
 *   KEY_ARROW_LEFT  – move cursor left within the line.
 *   KEY_ARROW_RIGHT – move cursor right within the line.
 *   KEY_ARROW_UP    – recall older history entry.
 *   KEY_ARROW_DOWN  – recall newer entry (or restore working line).
 *   '\n' / '\r'     – end of line.
 * Always NUL-terminates buf.  Reads at most (max - 1) characters.
 * --------------------------------------------------------------------------- */
void shell_readline(char *buf, size_t max)
{
    size_t len      = 0;
    size_t cur      = 0;             /* cursor position within buf            */
    size_t drawn    = 0;             /* chars rendered on screen (for erasure)*/
    int    hist_pos = -1;            /* -1 = not in history-navigation mode   */
    char   work[SHELL_MAX_INPUT];    /* in-progress line saved on first ↑     */

    buf[0]  = '\0';
    work[0] = '\0';

    /* Capture where the prompt ended.  VGA uses t_column/t_row (wraps at
     * VGA_WIDTH and resets t_row=0 when VESA is active).  VESA tracks its
     * own cursor which must be read separately to avoid the t_row=0 desync. */
    size_t   rl_col      = t_column;
    size_t   rl_row      = t_row;
    uint32_t vesa_rl_col = vesa_tty_is_ready() ? vesa_tty_get_col() : (uint32_t)t_column;
    uint32_t vesa_rl_row = vesa_tty_is_ready() ? vesa_tty_get_row() : (uint32_t)t_row;

    while (1) {
        unsigned char c = keyboard_getchar();

        if (c == '\n' || c == '\r') {
            /* Sync VGA tty state to end of line, then emit newline. */
            size_t end = rl_col + len;
            t_row      = rl_row + end / VGA_WIDTH;
            t_column   = end % VGA_WIDTH;
            if (vesa_tty_is_ready()) {
                uint32_t vcols = vesa_tty_get_cols();
                uint32_t vend  = vesa_rl_col + (uint32_t)len;
                vesa_tty_set_cursor(vend % vcols, vesa_rl_row + vend / vcols);
            }
            t_putchar('\n');  /* also writes '\n' to serial via tty.c */
            break;
        }

        if (c == '\b') {
            if (cur > 0) {
                for (size_t i = cur - 1; i < len - 1; i++)
                    buf[i] = buf[i + 1];
                len--;
                cur--;
                buf[len] = '\0';
                Serial_WriteChar('\b');
                size_t et = (drawn > len) ? drawn : len;
                drawn = len;
                readline_redraw(buf, len, cur, et,
                                rl_col, rl_row, vesa_rl_col, vesa_rl_row);
            }
            hist_pos = -1;
            continue;
        }

        /* ---- Inline cursor movement ---- */
        if (c == KEY_ARROW_LEFT) {
            if (cur > 0) {
                cur--;
                size_t cp = rl_col + cur;
                update_cursor(rl_row + cp / VGA_WIDTH, cp % VGA_WIDTH);
                if (vesa_tty_is_ready()) {
                    uint32_t vcols = vesa_tty_get_cols();
                    uint32_t vcp   = vesa_rl_col + (uint32_t)cur;
                    vesa_tty_set_cursor(vcp % vcols, vesa_rl_row + vcp / vcols);
                }
            }
            continue;
        }

        if (c == KEY_ARROW_RIGHT) {
            if (cur < len) {
                cur++;
                size_t cp = rl_col + cur;
                update_cursor(rl_row + cp / VGA_WIDTH, cp % VGA_WIDTH);
                if (vesa_tty_is_ready()) {
                    uint32_t vcols = vesa_tty_get_cols();
                    uint32_t vcp   = vesa_rl_col + (uint32_t)cur;
                    vesa_tty_set_cursor(vcp % vcols, vesa_rl_row + vcp / vcols);
                }
            }
            continue;
        }

        /* ---- History navigation ---- */
        if (c == KEY_ARROW_UP || c == KEY_ARROW_DOWN) {
            int new_pos;

            if (c == KEY_ARROW_UP) {
                if (hist_pos < 0) {
                    strncpy(work, buf, max - 1);
                    work[max - 1] = '\0';
                }
                new_pos = (hist_pos < 0) ? 0 : hist_pos + 1;
                if (new_pos >= s_hist_count)
                    new_pos = s_hist_count - 1;
            } else {
                new_pos = hist_pos - 1;
            }

            const char *entry;
            if (new_pos < 0) {
                entry    = work;
                hist_pos = -1;
            } else {
                entry = history_get(new_pos);
                if (!entry) continue;
                hist_pos = new_pos;
            }

            strncpy(buf, entry, max - 1);
            buf[max - 1] = '\0';
            len = strlen(buf);
            cur = len;
            size_t et2 = (drawn > len) ? drawn : len;
            drawn = len;
            readline_redraw(buf, len, cur, et2,
                            rl_col, rl_row, vesa_rl_col, vesa_rl_row);
            continue;
        }

        /* Ctrl+C: abort current line. */
        if (c == KEY_CTRL_C) {
            t_writestring("^C\n");
            buf[0] = '\0';
            return;
        }

        /* TTY focus gained: vtty_switch has already repainted the framebuffer
         * from this TTY's backing grid (vt_buf), which already contains the
         * full prompt + accumulated input.  Just drop the sentinel byte and
         * keep reading - no clear, no reprint, no redraw.  Linux VT behaviour. */
        if (c == KEY_FOCUS_GAIN) {
            continue;
        }

        /* Tab completion. */
        if (c == '\t') {
            /* Find word start (scan left for space or start of buf). */
            size_t ws = cur;
            while (ws > 0 && buf[ws - 1] != ' ') ws--;
            const char *word = buf + ws;
            size_t wlen = cur - ws;

            /* Is this the first (command) token? */
            int is_cmd = 1;
            for (size_t i = 0; i < ws; i++) {
                if (buf[i] != ' ') { is_cmd = 0; break; }
            }
            /* Path-style word ('/abs/...' or './rel/...' or contains a '/')
             * always completes against the VFS, regardless of token
             * position - bash treats `./fo<TAB>` and `ls /etc/fo<TAB>` the
             * same way: enumerate the named directory. */
            int is_path = 0;
            for (size_t i = 0; i < wlen; i++)
                if (word[i] == '/') { is_path = 1; break; }

            /* Collect matches using tab_vctx_t / tab_complete_cb defined above. */
            tab_vctx_t vctx;
            vctx.pfx    = word;
            vctx.pfxlen = wlen;
            vctx.n      = 0;
            int nmatches = 0;

            if (is_cmd && !is_path) {
                /* Bash-style first-token completion: built-ins + every ELF
                 * on the search path + entries in CWD whose name parses as
                 * an executable.  Order matters only for ranking duplicates;
                 * tab_app_cb dedupes by base name. */
                static const shell_cmd_entry_t *mods[] = {
                    help_cmds, display_cmds, system_cmds,
                    disk_cmds, fs_cmds, apps_cmds, NULL
                };
                for (int mi = 0; mods[mi] && vctx.n < 32; mi++) {
                    for (const shell_cmd_entry_t *e = mods[mi];
                         e->name && vctx.n < 32; e++) {
                        tab_complete_cb(e->name, 0, &vctx);
                    }
                }
                /* PATH dirs.  s_app_path entries always end in '/'; trim it
                 * before passing to vfs_complete for consistent normalisation. */
                for (int p = 0; s_app_path[p] && vctx.n < 32; p++) {
                    char dir[VFS_PATH_MAX];
                    size_t dlen = strlen(s_app_path[p]);
                    if (dlen >= sizeof(dir)) continue;
                    memcpy(dir, s_app_path[p], dlen);
                    if (dlen > 1 && dir[dlen - 1] == '/') dlen--;
                    dir[dlen] = '\0';
                    vfs_complete(dir, word, tab_app_cb, &vctx);
                }
                /* CWD: pick up `./foo` style executables typed without the
                 * leading "./".  Same .elf-stripping rule as PATH dirs. */
                if (vctx.n < 32)
                    vfs_complete(vfs_getcwd(), word, tab_app_cb, &vctx);
                nmatches = vctx.n;
            } else {
                /* Complete from VFS: split word into dir_part + file_prefix. */
                char dir_part[128];
                const char *file_prefix;
                const char *last_slash = NULL;
                for (size_t i = 0; i < wlen; i++)
                    if (word[i] == '/') last_slash = word + i;

                if (last_slash) {
                    size_t dlen = (size_t)(last_slash - word);
                    if (dlen == 0) {
                        dir_part[0] = '/'; dir_part[1] = '\0';
                    } else {
                        if (dlen > 127) dlen = 127;
                        memcpy(dir_part, word, dlen);
                        dir_part[dlen] = '\0';
                    }
                    file_prefix = last_slash + 1;
                } else {
                    strncpy(dir_part, vfs_getcwd(), 127);
                    dir_part[127] = '\0';
                    file_prefix = word;
                }

                vctx.pfx    = file_prefix;
                vctx.pfxlen = strlen(file_prefix);
                vfs_complete(dir_part, file_prefix, tab_complete_cb, &vctx);
                nmatches = vctx.n;
            }

            if (nmatches == 1) {
                /* Insert remainder of the single match. */
                size_t mlen = strlen(vctx.matches[0]);
                int    inserted_any = 0;
                for (size_t i = wlen; i < mlen && len < max - 1; i++) {
                    for (size_t j = len; j > cur; j--)
                        buf[j] = buf[j - 1];
                    buf[cur++] = vctx.matches[0][i];
                    len++;
                    inserted_any = 1;
                }
                /* Trailing affix: '/' for directories so the next TAB drills
                 * into the dir; ' ' for finished tokens (commands or files)
                 * so the user can immediately type the next argument.  Match
                 * bash's behaviour: only append when the cursor is at the
                 * end and nothing's already there.  Skip the space when we
                 * inserted nothing AND the next char is already a space - no
                 * point typing one twice. */
                if (vctx.is_dir[0] && len < max - 1) {
                    for (size_t j = len; j > cur; j--)
                        buf[j] = buf[j - 1];
                    buf[cur++] = '/';
                    len++;
                } else if (!vctx.is_dir[0] && len < max - 1 &&
                           (cur == len || buf[cur] != ' ')) {
                    /* Append space - even if no chars were inserted, so users
                     * of unique-prefix names (cat, cd) get visible feedback
                     * that the match succeeded. */
                    (void)inserted_any;
                    for (size_t j = len; j > cur; j--)
                        buf[j] = buf[j - 1];
                    buf[cur++] = ' ';
                    len++;
                }
                buf[len] = '\0';
                size_t et3 = (drawn > len) ? drawn : len;
                drawn = len;
                readline_redraw(buf, len, cur, et3,
                                rl_col, rl_row, vesa_rl_col, vesa_rl_row);
            } else if (nmatches > 1) {
                /* Print all matches, then redisplay prompt + line. */
                t_putchar('\n');
                for (int i = 0; i < nmatches; i++) {
                    t_writestring(vctx.matches[i]);
                    if (vctx.is_dir[i]) t_putchar('/');
                    t_putchar(' ');
                }
                t_putchar('\n');
                /* Reprint prompt and update readline anchor. */
                t_writestring(SHELL_USERNAME "@" SHELL_HOSTNAME " ");
                t_writestring(vfs_getcwd());
                t_writestring("~> ");
                rl_col = t_column;
                rl_row = t_row;
                if (vesa_tty_is_ready()) {
                    vesa_rl_col = vesa_tty_get_col();
                    vesa_rl_row = vesa_tty_get_row();
                }
                size_t et4 = (drawn > len) ? drawn : len;
                drawn = len;
                readline_redraw(buf, len, cur, et4,
                                rl_col, rl_row, vesa_rl_col, vesa_rl_row);
            }
            continue;
        }

        /* Accept printable ASCII; silently drop everything else. */
        if (c < 0x20 || c > 0x7E)
            continue;

        hist_pos = -1;

        if (len < max - 1) {
            for (size_t i = len; i > cur; i--)
                buf[i] = buf[i - 1];
            buf[cur] = c;
            len++;
            cur++;
            buf[len] = '\0';
            Serial_WriteChar(c);
            drawn = len;
            readline_redraw(buf, len, cur, len,
                            rl_col, rl_row, vesa_rl_col, vesa_rl_row);
        }
    }

    buf[len] = '\0';
}

/* ---------------------------------------------------------------------------
 * shell_parse – split line in-place into at most max_args tokens.
 *
 * Tokens are separated by spaces.  Returns the number of tokens found.
 * --------------------------------------------------------------------------- */
static int shell_parse(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < max_args) {
        /* Skip leading spaces. */
        while (*p == ' ')
            p++;

        if (!*p)
            break;

        argv[argc++] = p;

        /* Advance to the end of the token. */
        while (*p && *p != ' ')
            p++;

        if (*p)
            *p++ = '\0';
    }

    return argc;
}

/* ---------------------------------------------------------------------------
 * Command dispatch – module table pattern.
 *
 * Each category file exports a NULL-name-terminated shell_cmd_entry_t[].
 * To add a new command: add it to the appropriate category file's table.
 * To add a new category: create a new file and add its table pointer here.
 * --------------------------------------------------------------------------- */

static const shell_cmd_entry_t * const cmd_modules[] = {
    man_cmds,
    help_cmds,
    display_cmds,
    system_cmds,
    disk_cmds,
    fs_cmds,
    apps_cmds,
    fileops_cmds,
    NULL,
};

/* Try to exec at `path`; if that fails, try with ".elf" appended.  Returns
 * 1 if a binary was launched, 0 otherwise.  Used by both the path-prefix
 * branch (./foo, /cdrom/apps/foo) and the PATH lookup. */
static int try_exec_path(const char *path, int argc, char **argv)
{
    static char with_ext[VFS_PATH_MAX];
    if (vfs_file_exists(path)) {
        shell_exec_elf(path, argc, argv);
        return 1;
    }
    size_t plen = strlen(path);
    if (plen + 4 >= VFS_PATH_MAX)
        return 0;
    strncpy(with_ext, path, VFS_PATH_MAX - 1);
    strncpy(with_ext + plen, ".elf", 5);
    if (vfs_file_exists(with_ext)) {
        shell_exec_elf(with_ext, argc, argv);
        return 1;
    }
    return 0;
}

static int shell_dispatch(int argc, char **argv)
{
    for (int m = 0; cmd_modules[m]; m++) {
        for (int i = 0; cmd_modules[m][i].name; i++) {
            if (strcmp(cmd_modules[m][i].name, argv[0]) == 0) {
                cmd_modules[m][i].fn(argc, argv);
                return 1;
            }
        }
    }

    /* Path-style invocation: `/abs/path[.elf]` or `./relative[.elf]`.
     * Resolved by the VFS so `./foo` is interpreted relative to the CWD. */
    const char *cmd = argv[0];
    if (cmd[0] == '/' || (cmd[0] == '.' && cmd[1] == '/')) {
        if (try_exec_path(cmd, argc, argv))
            return 1;
        /* Fall through to "Unknown command" - the user explicitly named a
         * path; PATH lookup wouldn't make sense as a fallback. */
        return 0;
    }

    /* PATH lookup: try <dir><cmd>[.elf] for each entry in s_app_path. */
    static char path_buf[VFS_PATH_MAX];
    for (int p = 0; s_app_path[p]; p++) {
        size_t dlen = strlen(s_app_path[p]);
        size_t nlen = strlen(argv[0]);
        if (dlen + nlen + 4 >= VFS_PATH_MAX)
            continue;
        strncpy(path_buf, s_app_path[p], VFS_PATH_MAX - 1);
        strncpy(path_buf + dlen, argv[0], VFS_PATH_MAX - 1 - dlen);
        path_buf[dlen + nlen] = '\0';
        if (try_exec_path(path_buf, argc, argv))
            return 1;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * shell_print_prompt – print the interactive prompt showing the VFS CWD.
 *
 * Format:  root@makar /hd/boot~>
 * --------------------------------------------------------------------------- */
static void shell_print_prompt(void)
{
    t_writestring(SHELL_USERNAME "@" SHELL_HOSTNAME " ");
    t_writestring(vfs_getcwd());
    t_writestring("~> ");
}

/* ---------------------------------------------------------------------------
 * shell_run – infinite REPL loop.  Never returns.
 * --------------------------------------------------------------------------- */
void shell_run(void)
{
    char  buf[SHELL_MAX_INPUT];   /* stack-allocated: each task gets its own */
    char *argv[SHELL_MAX_ARGS];

    int slot = vtty_register();

    if (slot == 0) {
        terminal_set_colorscheme(SHELL_COLOR_VGA);

        /* Loading screen: render a progress bar that fills as bg ktest
         * completes each suite.  The wait is OUTSIDE the VESA conditional so
         * the VGA fallback also blocks the REPL until ktest_bg_done = 1.
         * On VESA we draw a 30-cell ASCII bar two rows below the logo; on
         * VGA we just yield silently (progress prints already go to serial
         * via ktest_bg_task's RUN macro). */
        if (vesa_tty_is_ready()) {
            vesa_tty_setcolor(SHELL_FG_RGB, SHELL_BG_RGB);
            vesa_tty_clear();
            vesa_blit_logo(SHELL_FG_RGB, SHELL_BG_RGB);

            const uint32_t bar_width = 30;
            const uint32_t bar_row   = vesa_tty_get_rows() - 3;
            uint32_t cols     = vesa_tty_get_cols();
            uint32_t bar_col  = (cols > bar_width + 2) ?
                                 (cols - bar_width - 2) / 2 : 0;

            /* Draw the empty frame once. */
            vesa_tty_put_at('[', bar_col, bar_row);
            for (uint32_t i = 0; i < bar_width; i++)
                vesa_tty_put_at('.', bar_col + 1 + i, bar_row);
            vesa_tty_put_at(']', bar_col + 1 + bar_width, bar_row);

            uint32_t last_filled = 0;
            uint32_t spinner_tick = 0;
            while (!ktest_bg_done) {
                int done  = ktest_bg_completed;
                int total = ktest_bg_total > 0 ? ktest_bg_total : 1;
                if (done > total) done = total;
                uint32_t filled = (uint32_t)((done * (int)bar_width) / total);
                while (last_filled < filled) {
                    vesa_tty_put_at('#',
                                    bar_col + 1 + last_filled, bar_row);
                    last_filled++;
                }
                /* Tiny spinner inside the right bracket so the screen looks
                 * alive even between suites. */
                static const char frames[] = { '|', '/', '-', '\\' };
                vesa_tty_put_at(frames[(spinner_tick / 6) & 3],
                                bar_col + 1 + bar_width + 2, bar_row);
                spinner_tick++;
                task_yield();
            }
            /* Fill the bar completely on success. */
            while (last_filled < bar_width) {
                vesa_tty_put_at('#', bar_col + 1 + last_filled, bar_row);
                last_filled++;
            }
            vesa_tty_put_at(' ', bar_col + 1 + bar_width + 2, bar_row);
            vesa_tty_clear();
        }

        /* Belt-and-braces wait for the VGA fallback path (and to cover the
         * fallthrough from the VESA branch above without re-checking). */
        while (!ktest_bg_done)
            task_yield();
        while (keyboard_poll()) {}

        t_writestring("Makar -- version " SHELL_VERSION
                      ", build: " BUILD_DATE " " BUILD_TIME "\n");
        t_writestring("The GCC/C++ sibling of Medli\n");
        t_writestring(COPYRIGHT "\n");
        t_writestring("Released under the BSD-3 Clause Clear license\n\n");
        t_writestring("Type 'help' for a list of commands.\n");
        t_writestring("Welcome back, " SHELL_USERNAME "!\n\n");

    } else {
        /* Non-primary TTYs: wait for bg tests, then poll until this slot
         * gains focus (vtty_switch sets vtty_current and sends KEY_FOCUS_GAIN).
         * Polling avoids a blocking keyboard_getchar() before ktest completes. */
        while (!ktest_bg_done)
            task_yield();
        while (!vtty_is_focused())
            task_yield();
        terminal_set_colorscheme(SHELL_COLOR_VGA);
        if (vesa_tty_is_ready()) {
            vesa_tty_setcolor(SHELL_FG_RGB, SHELL_BG_RGB);
            vesa_tty_clear();
        }
        /* Do NOT drain the input ring here.  The user may have typed a key
         * the same instant they hit Alt+Fn (single QEMU sendkey burst or a
         * fast typist on real hardware) - draining would swallow that first
         * keystroke.  shell_readline's KEY_FOCUS_GAIN handler drops the
         * sentinel byte cleanly; any real chars that arrived alongside it
         * stay queued for the readline loop. */
    }

    while (1) {
        shell_print_prompt();
        shell_readline(buf, SHELL_MAX_INPUT);

        /* !! - recall and re-execute the most recent history entry. */
        if (strcmp(buf, "!!") == 0) {
            const char *prev = history_get(0);
            if (!prev || !*prev) {
                t_setcolor(SHELL_ERROR_COLOR_VGA);
                t_writestring("!!: no previous command\n\n");
                t_setcolor(SHELL_COLOR_VGA);
                continue;
            }
            strncpy(buf, prev, SHELL_MAX_INPUT - 1);
            buf[SHELL_MAX_INPUT - 1] = '\0';
            t_writestring(buf);
            t_writestring("\n");
        }

        history_push(buf);

        int argc = shell_parse(buf, argv, SHELL_MAX_ARGS);
        if (argc == 0)
            continue;

        /* Expand wildcards (*, ?) against the VFS.  Storage arena lives on
         * this stack frame so expanded tokens are valid until the next
         * REPL iteration.  Large enough for ~32 typical-length paths. */
        static char glob_buf[1024];
        argc = shell_expand_globs(argc, argv, SHELL_MAX_ARGS,
                                  glob_buf, sizeof(glob_buf));

        if (!shell_dispatch(argc, argv)) {
            t_setcolor(SHELL_ERROR_COLOR_VGA);
            t_writestring("Unknown command '");
            t_writestring(argv[0]);
            t_writestring("' - try 'lsman'.\n\n");
            t_setcolor(SHELL_COLOR_VGA);
        }
    }
}
