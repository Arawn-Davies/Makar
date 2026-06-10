#include <stdio.h>

int main(int argc, char **argv)
{
    /* stderr is unbuffered in musl -> immediate write(2), which the smoke
     * driver surfaces on serial.  Proves main() ran + musl's write path works. */
    fprintf(stderr, "hello from musl libc (stderr, argc=%d)\n", argc);

    /* stdout is fully buffered for a non-tty -> flushed by exit()'s __stdio_exit.
     * Exercises the buffered-stdio + flush-on-exit path too. */
    printf("hello from musl libc (stdout, argc=%d)\n", argc);
    return 0;
}
