#ifndef _MAKAR_TCC_UNISTD_H
#define _MAKAR_TCC_UNISTD_H
typedef int ssize_t;
int    close(int fd);
int    unlink(const char *p);
long   read(int fd, void *b, unsigned n);
long   write(int fd, const void *b, unsigned n);
long   lseek(int fd, long o, int w);
#endif
