/* wget.elf -- userspace HTTP/HTTPS downloader.
 *
 * Fetches the URL entirely in userspace (web.c) over kernel TCP sockets:
 * http:// via a plain socket, https:// via BearSSL (TLS).  Follows 3xx
 * redirects; writes the body to a VFS path.  The kernel owns only TCP/IP now.
 *
 * Usage: wget <http[s]://host[:port]/path> [outfile]
 *        outfile defaults to /tmp/<basename>.
 */
#include "syscall.h"
#include "web.h"

static void puts1(const char *s) { unsigned n = 0; while (s[n]) n++; sys_write(1, s, n); }

static void putu(unsigned v)
{
    char b[12]; int i = 0;
    if (!v) { sys_write(1, "0", 1); return; }
    while (v) { b[i++] = (char)('0' + v % 10); v /= 10; }
    char o[12];
    for (int j = 0; j < i; j++) o[j] = b[i - 1 - j];
    sys_write(1, o, (unsigned)i);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts1("usage: wget <http://host[:port]/path> [outfile]\n");
        return 1;
    }
    const char *url = argv[1];

    char outbuf[256];
    const char *out;
    if (argc >= 3) {
        out = argv[2];
    } else {
        const char *base = url;
        for (const char *c = url; *c; c++)
            if (*c == '/') base = c + 1;
        if (!*base) base = "index.html";
        unsigned o = 0;
        const char *pre = "/tmp/";
        while (*pre && o < sizeof(outbuf) - 1) outbuf[o++] = *pre++;
        while (*base && o < sizeof(outbuf) - 1) outbuf[o++] = *base++;
        outbuf[o] = '\0';
        out = outbuf;
    }

    int r = web_fetch(url, out);
    if (r >= 0) {
        puts1("saved "); putu((unsigned)r); puts1(" bytes to "); puts1(out); puts1("\n");
        return 0;
    }
    if (r <= -100 && r > -600) {
        puts1("wget: HTTP status "); putu((unsigned)(-r)); puts1(" (not saved)\n");
        return 1;
    }
    if (r == -2) {
        puts1("wget: write failed (read-only path? use /tmp or a mounted disk)\n");
        return 1;
    }
    puts1("wget: failed -- bad URL, DNS/connect, or TLS handshake error\n");
    return 1;
}
