/*
 * mouseprobe.elf -- live text readout of the pointer input chain (ring-3).
 *
 * Re-reads /dev/mouse a few times a second and prints the snapshot, so you can
 * watch the input pipeline while moving the pointer and see exactly where it
 * stops on a host where the mouse appears dead:
 *
 *   irq12   -- IRQ12 (AUX) service-routine fires.  0 and not rising while you
 *              move the mouse => the controller isn't raising the mouse IRQ.
 *   bytes   -- AUX bytes routed to the mouse.  irq12 rising but bytes flat =>
 *              the controller fires but reports no AUX data (status/OBF quirk).
 *   packets -- complete 3-byte packets.  bytes rising but packets flat =>
 *              the 3-byte framing never syncs (see resync).
 *   events  -- decoded motion/button events; pos -- accumulated cursor.
 *
 * Loops until 'q'/'Q' or Ctrl-C.  Renders with ANSI so it works the same in the
 * text console and inside the GUI terminal.
 */

#include "syscall.h"

static unsigned int slen(const char *s) { unsigned int n = 0; while (s[n]) n++; return n; }
static void put_s(const char *s) { sys_write(1, s, slen(s)); }

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Non-blocking stdin so 'q' quits without stalling the live refresh
     * (Ctrl-C is delivered as SIGINT and terminates us the usual way). */
    sys_fcntl(0, F_SETFL, O_NONBLOCK);

    put_s("\033[2J");                       /* clear once up front */

    for (;;) {
        /* Fresh snapshot each frame: /dev/mouse renders the current counters
         * at open, so re-open rather than holding a stale buffer. */
        char buf[300];
        long n = -1;
        int fd = sys_open("/dev/mouse", O_RDONLY);
        if (fd >= 0) { n = sys_read(fd, buf, sizeof(buf) - 1); sys_close(fd); }
        if (n < 0) n = 0;
        buf[n] = '\0';

        put_s("\033[H");                    /* home */
        put_s("mouse probe -- move the pointer; 'q' or Ctrl-C to quit\r\n\r\n");
        if (n > 0) put_s(buf);
        else       put_s("(/dev/mouse unavailable)\r\n");
        put_s("\033[0J");                   /* wipe any stale trailing text */

        /* Poll for the quit key (drain whatever is buffered). */
        unsigned char c;
        while (sys_read(0, &c, 1) == 1) {
            if (c == 'q' || c == 'Q') {
                put_s("\033[2J\033[H");
                sys_fcntl(0, F_SETFL, 0);
                return 0;
            }
        }

        /* ~12 Hz refresh. */
        unsigned int t0 = sys_uptime();
        while ((unsigned int)(sys_uptime() - t0) < 8) sys_yield();
    }
}
