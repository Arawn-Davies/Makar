/*
 * rtc.c -- CMOS real-time clock reader.
 *
 * Factored out of procfs.c (the original /proc/rtc render path) so the
 * gettimeofday/clock_gettime syscalls and any future userland clock
 * surface share one decoder.  Behaviour matches the original render_rtc
 * exactly: drain UIP, read fields, decode BCD when status B bit 2 is
 * clear, mask the 12/24h indicator off the hour, assume 21st century.
 */

#include <kernel/rtc.h>
#include <kernel/asm.h>     /* outb / inb */

static inline uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static inline uint8_t bcd_to_bin(uint8_t v)
{
    return (uint8_t)(((v & 0xF0u) >> 4) * 10u + (v & 0x0Fu));
}

int rtc_read(rtc_time_t *out)
{
    if (!out) return -1;

    /* Drain any in-progress update so we don't read mid-tick.  Bounded
     * loop so a broken RTC can't hang us forever. */
    for (int spin = 0; spin < 1000000; spin++) {
        if (!(cmos_read(0x0A) & 0x80u)) break;
    }

    uint8_t sec   = cmos_read(0x00);
    uint8_t min   = cmos_read(0x02);
    uint8_t hour  = cmos_read(0x04);
    uint8_t day   = cmos_read(0x07);
    uint8_t mon   = cmos_read(0x08);
    uint8_t year  = cmos_read(0x09);
    uint8_t statB = cmos_read(0x0B);

    if (!(statB & 0x04u)) {
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        /* Hour high bit is 12/24 indicator in BCD mode; mask before
         * decoding so we don't treat the indicator as a digit. */
        hour = bcd_to_bin((uint8_t)(hour & 0x7Fu));
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        year = bcd_to_bin(year);
    }

    out->year = (uint16_t)(2000u + (uint32_t)year);
    out->mon  = mon;
    out->day  = day;
    out->hour = hour;
    out->min  = min;
    out->sec  = sec;
    return 0;
}

static inline void cmos_write(uint8_t reg, uint8_t val)
{
    outb(0x70, reg);
    outb(0x71, val);
}

static inline uint8_t bin_to_bcd(uint8_t v)
{
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

/* Write the CMOS RTC.  Halts updates (status-B SET bit) while writing the six
 * fields, encoding BCD when the clock is in BCD mode (status B bit 2 clear),
 * matching rtc_read's decode.  Assumes 24-hour mode (QEMU's default; the read
 * path only masks the 12/24h bit, so a 24h write round-trips cleanly).  Backs
 * SYS_SETTIME -- since gettimeofday reads the RTC live, this is the whole clock. */
int rtc_write(const rtc_time_t *t)
{
    if (!t) return -1;

    for (int spin = 0; spin < 1000000; spin++)
        if (!(cmos_read(0x0A) & 0x80u)) break;          /* wait out any update */

    uint8_t statB = cmos_read(0x0B);
    uint8_t sec  = t->sec, min = t->min, hour = t->hour;
    uint8_t day  = t->day, mon = t->mon;
    uint8_t year = (uint8_t)((t->year >= 2000u ? t->year - 2000u : 0u) % 100u);

    if (!(statB & 0x04u)) {                              /* BCD mode -> encode */
        sec=bin_to_bcd(sec); min=bin_to_bcd(min); hour=bin_to_bcd(hour);
        day=bin_to_bcd(day); mon=bin_to_bcd(mon); year=bin_to_bcd(year);
    }

    cmos_write(0x0B, (uint8_t)(statB | 0x80u));          /* SET: halt updates */
    cmos_write(0x00, sec);  cmos_write(0x02, min);  cmos_write(0x04, hour);
    cmos_write(0x07, day);  cmos_write(0x08, mon);  cmos_write(0x09, year);
    cmos_write(0x0B, statB);                             /* resume updates */
    return 0;
}

/* Days-since-epoch for the first of each month (non-leap). */
static const uint16_t s_mdays[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

static int is_leap(uint32_t y)
{
    return (y % 4u == 0 && y % 100u != 0) || (y % 400u == 0);
}

int rtc_unix_time(uint32_t *out_secs)
{
    rtc_time_t t;
    if (rtc_read(&t) != 0) return -1;

    /* Days from 1970-01-01 to first day of t.year.  Both years span
     * cleanly into 32 bits up to ~2106 -- plenty for current use. */
    uint32_t days = 0;
    for (uint32_t y = 1970; y < t.year; y++) {
        days += is_leap(y) ? 366u : 365u;
    }

    uint8_t mon = (t.mon >= 1 && t.mon <= 12) ? t.mon : 1;
    days += s_mdays[mon - 1];
    if (mon > 2 && is_leap(t.year)) days += 1;
    days += (t.day > 0) ? (uint32_t)(t.day - 1) : 0;

    uint32_t secs = days * 86400u
                  + (uint32_t)t.hour * 3600u
                  + (uint32_t)t.min  * 60u
                  + (uint32_t)t.sec;

    if (out_secs) *out_secs = secs;
    return 0;
}
