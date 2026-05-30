#ifndef _MAKAR_TCC_SYS_MMAN_H
#define _MAKAR_TCC_SYS_MMAN_H
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4
#define MAP_PRIVATE  2
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED   ((void *)-1)
void *mmap(void *a, unsigned int sz, int prot, int fl, int fd, long off);
int   munmap(void *a, unsigned int sz);
#endif
