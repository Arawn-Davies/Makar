#include <kernel/types.h>
//#include <kernel/system.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kernel/asm.h>
#include <kernel/serial.h>

#define COM1 0x3f8
#define COM2 0x2f8
#define COM3 0x3e8
#define COM4 0x2e8

int PORT;

/* See kernel/serial.h.  Defaults on so init/driver banners print to
 * COM1 during boot; kernel_main flips it off (unless test_mode) right
 * before the shell launches. */
int g_serial_verbose = 1;

enum ComPorts
{
	COMPort1 = 0x3f8,
	COMPort2 = 0x2f8,
	COMPort3 = 0x3e8,
	COMPort4 = 0x2e8
};

int serialReceived() {
   return inb(COM1 + 5) & 1;
}
 
char Serial_ReadChar() {
   while (serialReceived() == 0);
   return inb(COM1);
}

int isTransmitEmpty() {
   return inb(COM1 + 5) & 0x20;
}

//Writes a character over a serial connection
void Serial_WriteChar(char a) {
   while (isTransmitEmpty() == 0);
   outb(COM1,a);
}

//Prints a string over a serial connection
void Serial_WriteString(string a)
{
    while (*a != 0)
    {
        Serial_WriteChar(*a);
        a = a + 1;
    }
}

// Prints an unsigned 32-bit integer in decimal over a serial connection
void Serial_WriteDec(uint32_t n)
{
    char buf[11]; /* max uint32 is 4294967295 (10 digits) + NUL */
    int i = 10;
    buf[i] = '\0';
    if (n == 0) {
        buf[--i] = '0';
    } else {
        while (n > 0) {
            buf[--i] = '0' + (n % 10);
            n /= 10;
        }
    }
    Serial_WriteString(&buf[i]);
}

// Prints an unsigned 32-bit integer in hexadecimal (0xXXXXXXXX) over serial
void Serial_WriteHex(uint32_t n)
{
    static const char hexdigits[] = "0123456789ABCDEF";
    char buf[11]; /* "0x" + 8 hex digits + NUL */
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 7; i >= 0; i--) {
        buf[2 + i] = hexdigits[n & 0xF];
        n >>= 4;
    }
    buf[10] = '\0';
    Serial_WriteString(buf);
}

//Initialises the serial connection
void init_serial(int ComPort) 
{
	printf("Serial init start\n");
	PORT = ComPort;
	outb(PORT + 1, 0x00);    // Disable all interrupts
	outb(PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
	outb(PORT + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
	outb(PORT + 1, 0x00);    //                  (hi byte)
	outb(PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
	outb(PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
	outb(PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
	printf("Serial init done!\n");
	Serial_WriteString("Serial init done!\n");
}
