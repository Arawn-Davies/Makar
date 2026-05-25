#ifndef _KERNEL_RTC_H
#define _KERNEL_RTC_H

#include <stdint.h>

/*
 * Standard PC CMOS RTC at I/O ports 0x70 (index) + 0x71 (data).
 * Decoded BCD/24h, century clamped to 2000+ (see rtc.c).
 */
typedef struct rtc_time {
    uint16_t year;   /* full year, e.g. 2026 */
    uint8_t  mon;    /* 1..12 */
    uint8_t  day;    /* 1..31 */
    uint8_t  hour;   /* 0..23 */
    uint8_t  min;    /* 0..59 */
    uint8_t  sec;    /* 0..59 */
} rtc_time_t;

/* Drain any in-progress update, then read + decode the CMOS RTC.
 * Returns 0 on success (always succeeds today). */
int rtc_read(rtc_time_t *out);

/* Convenience: read RTC and convert to seconds since 1970-01-01 UTC.
 * Returns 0 on success. */
int rtc_unix_time(uint32_t *out_secs);

#endif /* _KERNEL_RTC_H */
