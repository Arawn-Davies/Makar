/*
 * cp.c -- userspace file copy utility.
 *
 * Uses SYS_OPEN/SYS_READ to read the source into a stack buffer,
 * then SYS_WRITE_FILE to write it to the destination.
 * Maximum file size: 64 KiB (SYSCALL_FILE_MAX).
 */

#include "syscall.h"

static void write_str(const char *s)
{
    unsigned int len = 0;
    while (s[len]) len++;
    sys_write(1, s, len);
}

/* 64 KiB static buffer - matches the kernel's SYSCALL_FILE_MAX. */
static unsigned char s_buf[65536];

int main(int argc, char **argv)
{
    if (argc < 3) {
        write_str("Usage: cp <src> <dst>\n");
        sys_exit(1);
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    int fd = sys_open(src, O_RDONLY);
    if (fd < 0) {
        write_str("cp: cannot open '");
        write_str(src);
        write_str("'\n");
        sys_exit(1);
    }

    long n = sys_read(fd, s_buf, sizeof(s_buf));
    sys_close(fd);

    if (n < 0) {
        write_str("cp: read error on '");
        write_str(src);
        write_str("'\n");
        sys_exit(1);
    }

    if (sys_write_file(dst, s_buf, (unsigned int)n) != 0) {
        write_str("cp: cannot write '");
        write_str(dst);
        write_str("'\n");
        sys_exit(1);
    }

    sys_exit(0);
    return 0;
}
