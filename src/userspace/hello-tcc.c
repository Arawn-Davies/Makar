/*
 * hello-tcc.c -- canonical "Hello, TCC" sample, kept self-contained.
 *
 * This source is installed at /usr/share/examples/hello-tcc.c so the
 * the in-guest test drivers can compile a known-good source
 * with tcc on a running Makar guest, exec the output, and assert the
 * greeting on serial.  Avoids the libc shim so TCC isn't asked to
 * resolve stdio.h / malloc.h before the bring-up is proven.
 */
#include "syscall.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    sys_write(2, "Hello, TCC\n", 11);
    return 0;
}
