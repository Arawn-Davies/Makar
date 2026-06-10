#ifndef _TKEY_H
#define _TKEY_H

/*
 * tkey.h -- read one key from the controlling terminal in a way that works in
 * BOTH a text VT (fd 0 = the kernel keyboard) AND inside an mxterm window
 * (fd 0 = a pipe).  We set fd 0 non-blocking once and read raw bytes: those
 * carry the same KEY_* sentinels (0x80+) the kernel emits for arrows / function
 * keys, and mxterm forwards the identical byte over the pipe.  This replaces
 * sys_getkey(), which only ever reads the physical keyboard and is therefore
 * deaf when the app runs inside an mxterm window.
 */
#include "syscall.h"

static int tkey_get(void)            /* blocking: returns a byte (0..255) or -1 (EOF) */
{
    static int armed = 0;
    if (!armed) { sys_fcntl(0, F_SETFL, O_NONBLOCK); armed = 1; }
    unsigned char c;
    for (;;) {
        long n = sys_read(0, &c, 1);
        if (n == 1) return (int)c;
        if (n == 0) return -1;       /* terminal closed */
        sys_yield();
    }
}

#endif /* _TKEY_H */
