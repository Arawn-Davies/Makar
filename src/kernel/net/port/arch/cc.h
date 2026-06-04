#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>
#include <kernel/debug.h>
#include <kernel/serial.h>

#define BYTE_ORDER LITTLE_ENDIAN

#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_CTYPE_H 1
#define LWIP_NO_UNISTD_H 1
#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "u"

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_FLD_8(x) PACK_STRUCT_FIELD(x)
#define PACK_STRUCT_FLD_S(x) PACK_STRUCT_FIELD(x)

#define LWIP_PLATFORM_DIAG(x) do { lwip_platform_diag x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { Serial_WriteString("lwip assert: "); Serial_WriteString(x); Serial_WriteString("\n"); KPANIC("lwip assert"); } while (0)

void lwip_platform_diag(const char *fmt, ...);
int atoi(const char *s);

typedef unsigned long sys_prot_t;

#endif
