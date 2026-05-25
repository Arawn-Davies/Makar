/*
 * stdarg.h -- tiny i386 varargs support for freestanding userspace.
 *
 * Makar's ring-3 ABI is i386 cdecl, so variadic arguments live on the
 * caller stack and va_list is a byte pointer.
 */
#ifndef _USERSPACE_STDARG_H
#define _USERSPACE_STDARG_H

typedef char *va_list;

#define va_start(ap, last) ((ap) = ((char *)&(last)) + ((sizeof(last) + 3u) & ~3u))
#define va_arg(ap, type)   ((ap) += ((sizeof(type) + 3u) & ~3u), \
                            *(type *)((ap) - ((sizeof(type) + 3u) & ~3u)))
#define va_copy(dst, src)  ((dst) = (src))
#define va_end(ap)         ((void)(ap))

typedef va_list __gnuc_va_list;

#endif /* _USERSPACE_STDARG_H */
