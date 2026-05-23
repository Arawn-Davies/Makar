/* setjmp / longjmp -- i386 SysV ABI.  Saves callee-saved regs + the
 * caller's return address; restores them on longjmp and resumes as if
 * setjmp() had just returned `val` (or 1 if val == 0).
 *
 * jmp_buf is unsigned int[6]:
 *   0: ebx   1: esi   2: edi   3: ebp   4: esp (caller's, before our ret addr)
 *   5: eip   (caller's return address)
 */
    .text
    .global setjmp
    .type   setjmp, @function
setjmp:
    movl    4(%esp), %eax       /* env */
    movl    %ebx, 0(%eax)
    movl    %esi, 4(%eax)
    movl    %edi, 8(%eax)
    movl    %ebp, 12(%eax)
    leal    4(%esp), %ecx       /* caller's ESP = our ESP+4 (skip ret addr) */
    movl    %ecx, 16(%eax)
    movl    (%esp), %ecx        /* return address */
    movl    %ecx, 20(%eax)
    xorl    %eax, %eax          /* return 0 on direct call */
    ret
    .size   setjmp, .-setjmp

    .global longjmp
    .type   longjmp, @function
longjmp:
    movl    4(%esp), %edx       /* env */
    movl    8(%esp), %eax       /* val */
    testl   %eax, %eax
    jne     1f
    incl    %eax                /* val == 0 -> return 1 (POSIX) */
1:
    movl    0(%edx),  %ebx
    movl    4(%edx),  %esi
    movl    8(%edx),  %edi
    movl    12(%edx), %ebp
    movl    16(%edx), %esp
    movl    20(%edx), %ecx      /* saved return address */
    jmp     *%ecx               /* resume in caller of setjmp() */
    .size   longjmp, .-longjmp
