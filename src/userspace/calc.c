#include "syscall.h"

/* ---------- I/O helpers ---------- */

static void write_str(const char *s)
{
    unsigned int len = 0;
    while (s[len]) len++;
    sys_write(1, s, len);
}

static void write_char(char c) { sys_write(1, &c, 1); }

static void write_int(long n)
{
    if (n < 0) { write_char('-'); n = -n; }
    if (n == 0) { write_char('0'); return; }
    char buf[20];
    int i = 0;
    while (n > 0) { buf[i++] = '0' + (int)(n % 10); n /= 10; }
    for (int j = 0; j < i / 2; j++) {
        char t = buf[j]; buf[j] = buf[i-1-j]; buf[i-1-j] = t;
    }
    sys_write(1, buf, (unsigned int)i);
}

/* ---------- recursive-descent parser ----------
 *
 * Grammar:
 *   expr  = term  (('+' | '-') term)*
 *   term  = unary (('*' | '/' | '%') unary)*
 *   unary = '-' unary | factor
 *   factor = '(' expr ')' | NUMBER
 */

static const char *pos;
static int err;

static void set_err(const char *msg) { if (!err) { write_str(msg); err = 1; } }

static void skip_ws(void) { while (*pos == ' ' || *pos == '\t') pos++; }

static long parse_expr(void);

static long parse_factor(void)
{
    skip_ws();
    if (*pos == '(') {
        pos++;
        long v = parse_expr();
        skip_ws();
        if (*pos == ')') pos++;
        else set_err("error: unmatched '('\n");
        return v;
    }
    if (*pos < '0' || *pos > '9') { set_err("error: expected a number\n"); return 0; }
    long v = 0;
    while (*pos >= '0' && *pos <= '9') v = v * 10 + (*pos++ - '0');
    return v;
}

static long parse_unary(void)
{
    skip_ws();
    if (*pos == '-') { pos++; return -parse_unary(); }
    if (*pos == '+') { pos++; return  parse_unary(); }
    return parse_factor();
}

static long parse_term(void)
{
    long v = parse_unary();
    for (;;) {
        skip_ws();
        if (err) return 0;
        char op = *pos;
        if (op != '*' && op != '/' && op != '%') break;
        pos++;
        long r = parse_unary();
        if (err) return 0;
        if (op == '*') { v *= r; }
        else if (r == 0) { set_err("error: division by zero\n"); return 0; }
        else if (op == '/') { v /= r; }
        else { v %= r; }
    }
    return v;
}

static long parse_expr(void)
{
    long v = parse_term();
    for (;;) {
        skip_ws();
        if (err) return 0;
        char op = *pos;
        if (op != '+' && op != '-') break;
        pos++;
        long r = parse_term();
        if (err) return 0;
        v = (op == '+') ? v + r : v - r;
    }
    return v;
}

/* ---------- main REPL ---------- */

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Read one line char-at-a-time with local echo + backspace.  Mirrors sh.elf's
 * readline: sys_getkey() returns raw keys (no kernel echo, no line buffering)
 * and works identically on the classic VT and inside the GUI terminal, where
 * stdin is a pipe with no line discipline -- a plain sys_read(0) there returns
 * one byte per keystroke, so a cooked line read evaluated after every key.
 * Returns the length, or -1 on EOF (Ctrl-D / pipe closed). */
static int read_line(char *buf, int cap)
{
    int len = 0;
    for (;;) {
        int c = sys_getkey();
        if (c < 0) return (len > 0) ? len : -1;    /* EOF */
        unsigned char ch = (unsigned char)c;
        if (ch == '\r' || ch == '\n') { write_char('\n'); buf[len] = '\0'; return len; }
        if (ch == 0x04) return (len > 0) ? len : -1;            /* Ctrl-D */
        if (ch == 0x08 || ch == 0x7F) {                         /* backspace */
            if (len > 0) { len--; write_str("\b \b"); }
            continue;
        }
        if (ch < 0x20 || ch >= 0x80) continue;     /* skip ctrl/arrow sentinels */
        if (len < cap - 1) { buf[len++] = (char)ch; write_char((char)ch); }
    }
}

int main(void)
{
    char line[256];
    for (;;) {
        write_str("> ");
        int n = read_line(line, sizeof(line));
        if (n < 0) break;

        const char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0') continue;
        if (str_eq(s, "quit") || str_eq(s, "exit")) break;

        pos = s;
        err = 0;
        long result = parse_expr();

        if (!err) {
            skip_ws();
            if (*pos != '\0') {
                write_str("error: unexpected '");
                write_char(*pos);
                write_str("'\n");
            } else {
                write_int(result);
                write_char('\n');
            }
        }
    }

    return 0;
}
