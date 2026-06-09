/*
 * about.elf -- print Makar system information to stdout (ring-3).
 *
 * The CLI counterpart to the graphical mxabout panel: a plain text-mode tool
 * that works identically in the classic text console and the GUI terminal
 * (it only writes stdout, which the terminal bridges).  Reads the synthetic
 * /proc files the kernel already renders -- platform (hypervisor), CPU brand,
 * bound GPU/NIC drivers, and memory -- so it stays a thin formatter with no
 * privileged syscalls of its own.
 */

#include "syscall.h"

static unsigned int slen(const char *s) { unsigned int n = 0; while (s[n]) n++; return n; }
static void put_s(const char *s) { sys_write(1, s, slen(s)); }

/* Stream a whole /proc file to stdout. */
static void cat(const char *path)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return;
    char buf[512];
    long n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0)
        sys_write(1, buf, (unsigned int)n);
    sys_close(fd);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    put_s("==== About Makar ====\n\n");
    cat("/proc/uname");
    put_s("\n--- CPU & platform ---\n");
    cat("/proc/cpuinfo");
    put_s("\n--- Memory ---\n");
    cat("/proc/meminfo");
    return 0;
}
