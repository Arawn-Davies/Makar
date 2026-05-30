#ifndef _MAKAR_TCC_SYS_TIME_H
#define _MAKAR_TCC_SYS_TIME_H
/* struct timeval comes from <syscall.h> (pulled in via <stdio.h> here). */
#include "syscall.h"
int gettimeofday(struct timeval *tv, void *tz);
#endif
