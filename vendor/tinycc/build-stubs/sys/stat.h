#ifndef _MAKAR_TCC_SYS_STAT_H
#define _MAKAR_TCC_SYS_STAT_H
#include <syscall.h>
int stat(const char *p, struct stat *s);
int fstat(int fd, struct stat *s);
int chmod(const char *p, unsigned int m);
#endif
