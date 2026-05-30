/*
 * freestanding.c -- minimal kernel-shaped translation unit for the
 * slice-5c-rung-2 link experiment.
 *
 * Goal: prove that the in-OS TCC can produce a freestanding ELF
 * (no crt, no libc) suitable for use as a kernel binary.  We don't
 * actually boot the result -- we only assert that TCC's link step
 * accepts `-nostdlib` and produces a valid ELF with the entry
 * point we asked for.  Booting the produced image is the next
 * rung up the ladder (slice 5d/5e -- mkmakar.sh + qemu).
 *
 * Lives in src/userspace/ so it ships on the ISO via the existing
 * `cp -r src/. isodir/src/` step in iso.sh -- the Makefile doesn't
 * compile it (not in PROGS) so it stays purely as input source
 * for the in-OS TCC tests in libc-tcc.sh.
 */

/* No headers at all -- freestanding C.  Just primitives. */

/* Kernel entry.  Returns 42 (the canonical "test ran" sentinel).
 * The `volatile` write to a sink stops TCC from optimising the body
 * away to a bare `mov eax, 42; ret`. */
int kmain(void)
{
    volatile int sink = 0;
    for (int i = 0; i < 4; i++) sink += i;
    return 42 + sink;
}

/* Entry symbol -- ENTRY(_start) in linker.ld points here on a real
 * Multiboot boot.  We can't generate a Multiboot 2 header from C
 * (it needs precise byte-level layout, typically emitted by asm),
 * so this is a stub _start without the header.  Sufficient for the
 * link test; not sufficient for an actually-bootable kernel. */
void _start(void)
{
    int rc = kmain();
    /* Spin -- a real kernel would hand off to its scheduler here. */
    volatile int *flag = (volatile int *)0x1000;
    *flag = rc;
    while (1) { }
}
