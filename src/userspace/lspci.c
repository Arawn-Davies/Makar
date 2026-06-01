#include "syscall.h"

static char buf[4096];

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int n = sys_pci_info(buf, (unsigned int)sizeof(buf));
    if (n <= 0) {
        const char *msg = "lspci: no PCI devices found\n";
        sys_write(1, msg, 28);
        return 1;
    }
    sys_write(1, buf, (unsigned int)n);
    return 0;
}
