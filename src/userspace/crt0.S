.section .text
.global _start
_start:
    /*
     * Linux i386 / ELKS / Fuzix ABI at process entry:
     *
     *   [esp + 0]            = argc
     *   [esp + 4]            = argv[0]   (char *, first of an inline array)
     *   [esp + 4 + argc*4]   = NULL      (argv[argc] sentinel)
     *   [esp + 4 + (argc+1)*4] = envp[0] (NULL in our case)
     *
     * argv and envp are NOT pointers-to-arrays; they ARE the arrays.
     * So char **argv = esp+4 and char **envp = esp+4+(argc+1)*4.
     */
    movl  (%esp), %eax       /* argc                             */
    leal 4(%esp), %ecx       /* &argv[0] - this IS char **argv   */
    /* envp starts after argv[argc]=NULL, i.e. 4(%ecx,%eax,4) */
    leal 4(%ecx,%eax,4), %edx  /* char **envp                   */

    push  %edx               /* envp  (third arg to main)        */
    push  %ecx               /* argv  (second arg to main)       */
    push  %eax               /* argc  (first arg to main)        */
    call  main

    /* Use main's return value as the exit status. */
    movl  %eax, %ebx
    movl  $1,   %eax         /* SYS_EXIT */
    int   $0x80
1:  jmp   1b
