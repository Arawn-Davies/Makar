#include <kernel/timer.h>
#include <kernel/serial.h>
#include <lwip/sys.h>
#include <stdarg.h>

int atoi(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

u32_t sys_now(void)
{
    return timer_get_ticks() * 10u;
}

void lwip_platform_diag(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; p && *p; p++) {
        if (*p != '%') {
            Serial_WriteChar(*p);
            continue;
        }
        p++;
        if (*p == '%') {
            Serial_WriteChar('%');
        } else if (*p == 's') {
            const char *s = va_arg(ap, const char *);
            Serial_WriteString((char *)(s ? s : "(null)"));
        } else if (*p == 'd' || *p == 'u') {
            Serial_WriteDec(va_arg(ap, uint32_t));
        } else if (*p == 'x') {
            Serial_WriteHex(va_arg(ap, uint32_t));
        }
    }
    va_end(ap);
}
