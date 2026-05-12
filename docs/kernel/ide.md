# IDE Driver (`ide.c` / `ide.h`)

## Overview

`ide.c` implements a minimal ATA PIO (Programmed I/O) driver for the Makar
kernel.  It supports 28-bit LBA sector reads and writes over two ATA channels
(primary and secondary), giving access to up to four drives (primary master,
primary slave, secondary master, secondary slave).

All transfers are polling-based - no DMA, no IRQ-driven I/O.  ATAPI (CD-ROM)
devices are detected and labelled but sector read/write is not supported for
them.

---

## Hardware Registers

Each ATA channel exposes two I/O port ranges:

| Channel   | Command block base | Control block base |
|-----------|-------------------|--------------------|
| Primary   | `0x1F0`           | `0x3F6`            |
| Secondary | `0x170`           | `0x376`            |

Command-block register offsets:

| Offset | Read            | Write    |
|--------|-----------------|----------|
| `+0`   | Data (16-bit)   | Data     |
| `+1`   | Error           | Features |
| `+2`   | Sector Count    | -        |
| `+3`   | LBA bits 0–7    | -        |
| `+4`   | LBA bits 8–15   | -        |
| `+5`   | LBA bits 16–23  | -        |
| `+6`   | Drive/Head      | -        |
| `+7`   | Status          | Command  |

The control block base (`+0`) holds the alternate-status register (read) or
the device-control register (write, used to disable IRQs via bit 1 `nIEN`).

---

## Drive Indices

```
Index 0 – primary   master
Index 1 – primary   slave
Index 2 – secondary master
Index 3 – secondary slave
```

---

## Public API

### `void ide_init(void)`

Scans both channels, runs `IDENTIFY DEVICE` (or `IDENTIFY PACKET DEVICE` for
ATAPI) on every slot, and populates the internal drive table.

Must be called once during kernel initialisation, **after** interrupts are
disabled for the ATA channels (which this function does itself via the
control-block `nIEN` bit).

Before probing, `ide_init` issues a software reset (SRST, ATA spec §9.1) on
both channels: assert bit 2 of the Device Control register for ≥5 µs, then
deassert and wait ~100 µs for drives to recalibrate.  This is required because
GRUB leaves the primary channel in a transient state after loading the kernel;
without the reset, the first `IDENTIFY` status read returns `0x00` and drive 0
is silently skipped.

---

### `int ide_read_sectors(uint8_t drive_num, uint32_t lba, uint8_t count, void *buf)`

Reads `count` 512-byte sectors starting at 28-bit LBA address `lba` from
drive `drive_num` into `buf`.

| Return value | Meaning                                         |
|--------------|-------------------------------------------------|
| `0`          | Success                                         |
| `-1`         | Drive index out of range or drive not present   |
| `-2`         | Drive is ATAPI - not supported by PIO read      |
| `1`          | ATA error bit set during transfer               |
| `2`          | Drive fault during transfer                     |
| `3`          | DRQ not asserted after command                  |

---

### `int ide_write_sectors(uint8_t drive_num, uint32_t lba, uint8_t count, const void *buf)`

Writes `count` 512-byte sectors from `buf` to drive `drive_num` starting at
LBA `lba`.  A cache-flush command is issued automatically after the last
sector.

Return values are identical to `ide_read_sectors`.

---

### `const ide_drive_t *ide_get_drive(uint8_t drive_num)`

Returns a pointer to the drive descriptor for `drive_num` (0–3), or `NULL` if
the index is out of range.  Callers must check `drive->present` before using
any other field.

---

## `ide_drive_t` Structure

```c
typedef struct {
    uint8_t  present;       /* 1 if a drive exists at this index          */
    uint8_t  channel;       /* 0 = primary, 1 = secondary                 */
    uint8_t  drive;         /* 0 = master,  1 = slave                     */
    uint8_t  type;          /* IDE_TYPE_ATA or IDE_TYPE_ATAPI             */
    uint16_t signature;     /* Device type word from IDENTIFY             */
    uint16_t capabilities;  /* Capabilities word from IDENTIFY            */
    uint32_t command_sets;  /* Supported command sets from IDENTIFY       */
    uint32_t size;          /* Size in 512-byte sectors                   */
    char     model[41];     /* Model string (NUL-terminated, trimmed)     */
} ide_drive_t;
```

---

## Shell Commands

Two shell commands expose the driver to the interactive kernel shell:

### `lsdisks`

Lists all detected ATA/ATAPI drives with their type, size (in MiB), and model
string.  Example output:

```
drive 0: [primary master] ATA  20480 MiB  "QEMU HARDDISK"
```

### `readsector <drive> <lba>`

Reads one 512-byte sector from the given drive at the given LBA address and
prints a hex dump to the terminal.  Both arguments can be decimal or
`0x`-prefixed hexadecimal.

Example:

```
untitled> readsector 0 0
Sector 0 of drive 0:
0000:  EB 63 90 00 ...
```

---

## Limitations

* Only 28-bit LBA is used for read/write.  Drives larger than 128 GiB can be
  detected (their 64-bit sector count is truncated to 32 bits) but only the
  first 128 GiB is accessible.
* No DMA support.  PIO is slower than DMA for large transfers.
* No IRQ-driven transfers; the driver busy-waits on status bits.
* ATAPI (CD-ROM/DVD) drives are detected but not readable through this driver.
