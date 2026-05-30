#ifndef _MAKAR_TCC_FCNTL_H
#define _MAKAR_TCC_FCNTL_H
#include <syscall.h>   /* O_* live here in the shim */
int open(const char *p, int f, ...);
#endif
