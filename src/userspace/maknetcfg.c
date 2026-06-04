#include "syscall.h"

static char buf[1024];

static unsigned int slen(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

static void put_s(const char *s)
{
    sys_write(1, s, slen(s));
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        put_s("Usage: maknetcfg\n");
        put_s("Shows the active Ethernet netdev and static QEMU-slirp defaults.\n");
        return 1;
    }

    (void)argv;
    int n = sys_net_info(buf, (unsigned int)sizeof(buf));
    if (n <= 0) {
        put_s("maknetcfg: no network information available\n");
        return 1;
    }
    sys_write(1, buf, (unsigned int)n);
    return 0;
}
