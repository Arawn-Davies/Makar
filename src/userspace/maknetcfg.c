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

static int streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int show_info(void)
{
    int n = sys_net_info(buf, (unsigned int)sizeof(buf));
    if (n <= 0) {
        put_s("maknetcfg: no network information available\n");
        return 1;
    }
    sys_write(1, buf, (unsigned int)n);
    return 0;
}

static void usage(void)
{
    put_s("Usage: maknetcfg [command]\n");
    put_s("\n");
    put_s("Network configuration for the active Ethernet interface (eth0).\n");
    put_s("\n");
    put_s("Commands:\n");
    put_s("  (none)      show Ethernet/IPv4/DHCP/gateway/DNS state (ipconfig-style)\n");
    put_s("  release     stop DHCP and restore the static slirp fallback address\n");
    put_s("  renew       restart DHCP, falling back to static if no lease arrives\n");
    put_s("  flush-dns   clear the lwIP DNS resolver cache\n");
    put_s("  help        show this help (also -h, --help)\n");
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return show_info();

    if (argc == 2 && (streq(argv[1], "help") ||
                      streq(argv[1], "-h") || streq(argv[1], "--help"))) {
        usage();
        return 0;
    }

    if (argc != 2) {
        usage();
        return 1;
    }

    if (streq(argv[1], "release")) {
        if (sys_net_ctl(NET_CTL_DHCP_RELEASE) != 0) {
            put_s("maknetcfg: DHCP release failed\n");
            return 1;
        }
        put_s("DHCP lease released.\n\n");
        return show_info();
    }

    if (streq(argv[1], "renew")) {
        if (sys_net_ctl(NET_CTL_DHCP_RENEW) != 0) {
            put_s("maknetcfg: DHCP renew failed\n");
            return 1;
        }
        put_s("DHCP renew completed.\n\n");
        return show_info();
    }

    if (streq(argv[1], "flush-dns")) {
        if (sys_net_ctl(NET_CTL_DNS_FLUSH) != 0) {
            put_s("maknetcfg: DNS flush failed\n");
            return 1;
        }
        put_s("DNS resolver cache flushed.\n\n");
        return show_info();
    }

    usage();
    return 1;
}
