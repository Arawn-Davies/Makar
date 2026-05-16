---
title: serial
parent: Kernel subsystems
---

# serial - Serial port (UART) driver

**Header:** `kernel/include/kernel/serial.h`  
**Source:** `kernel/arch/i386/serial.c`

Drives the 16550-compatible UARTs for serial I/O.  In development builds the
serial port is the primary logging channel; all kernel subsystems write
progress and error messages via the `KLOG` family of macros.

---

## Port address constants

| Constant | Value | Description |
|---|---|---|
| `COM1` | `0x3F8` | First serial port (standard). |
| `COM2` | `0x2F8` | Second serial port. |
| `COM3` | `0x3E8` | Third serial port. |
| `COM4` | `0x2E8` | Fourth serial port. |

---

## Logging macros

These macros are defined in the header and conditioned on `DEV_BUILD`.

| Macro | Release build | Dev build |
|---|---|---|
| `KLOG(msg)` | no-op | `Serial_WriteString(msg)` |
| `KLOG_DEC(n)` | no-op | `Serial_WriteDec(n)` |
| `KLOG_HEX(n)` | no-op | `Serial_WriteHex(n)` |

Use `KLOG` instead of calling serial functions directly so that all debug
output is automatically stripped from release builds.

---

## Functions

### `init_serial`

```c
void init_serial(int ComPort);
```

Initialise the UART at the given port address.  Configures the divisor for
38 400 baud, 8-N-1 framing, enables the FIFO with a 14-byte threshold, and
activates RTS/DSR.

| Parameter | Description |
|---|---|
| `ComPort` | Base I/O address of the port (e.g. `COM1`). |

### `Serial_ReadChar`

```c
char Serial_ReadChar(void);
```

Block until a byte is available in the UART receive buffer, then return it.

### `Serial_WriteChar`

```c
void Serial_WriteChar(char a);
```

Block until the UART transmit buffer is empty, then send byte `a`.

### `Serial_WriteString`

```c
void Serial_WriteString(string a);
```

Send the null-terminated string `a` over the serial port one byte at a time.

### `Serial_WriteDec`

```c
void Serial_WriteDec(uint32_t n);
```

Format `n` as a decimal string and send it over the serial port.

### `Serial_WriteHex`

```c
void Serial_WriteHex(uint32_t n);
```

Format `n` as `0xXXXXXXXX` (eight uppercase hex digits) and send it over the
serial port.
