#ifndef _KERNEL_SERIAL_H_
#define _KERNEL_SERIAL_H_

#include <kernel/types.h>
#include <kernel/asm.h>

#define COM1 0x3f8
#define COM2 0x2f8
#define COM3 0x3e8
#define COM4 0x2e8

void init_serial(int ComPort);
char Serial_ReadChar();
void Serial_WriteChar(char a);
void Serial_WriteString(string a);
void Serial_WriteDec(uint32_t n);
void Serial_WriteHex(uint32_t n);

/*
 * g_serial_verbose - when nonzero, t_putchar mirrors every user-tty
 * character to COM1.  Defaults to 1 so boot diagnostics are visible on
 * serial.  kernel_main flips it to 0 just before launching the shell
 * (Linux-style: dmesg shows boot+drivers+errors, not tty output) unless
 * test_mode is set on the cmdline, in which case it stays 1 so CI can
 * grep the mirror.  Explicit Serial_*, KLOG, kpanic etc. bypass this
 * flag - they are kernel diagnostics, not tty output.
 */
extern int g_serial_verbose;

/*
 * Lightweight serial-logging macros for development/debug builds.
 *
 * Include this header and compile with -DDEV_BUILD to enable verbose serial
 * output from every kernel subsystem.  In release builds these expand to
 * no-ops so there is no runtime overhead.
 */
#ifdef DEV_BUILD
#  define KLOG(msg)    Serial_WriteString(msg)
#  define KLOG_DEC(n)  Serial_WriteDec(n)
#  define KLOG_HEX(n)  Serial_WriteHex(n)
#else
#  define KLOG(msg)    ((void)0)
#  define KLOG_DEC(n)  ((void)0)
#  define KLOG_HEX(n)  ((void)0)
#endif

#endif
