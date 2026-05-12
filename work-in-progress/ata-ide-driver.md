# ATA/IDE driver - PIO-mode disk access

> **Status:** complete
> **Branch:** landed on `main`

## Summary

PIO-mode ATA/IDE driver providing block-level read/write access to up to
four drives (two channels, master + slave each).  Backs the FAT32 and
ISO 9660 VFS drivers.

## Implemented

- [x] Detect primary and secondary ATA channels (master + slave)
- [x] Software reset (SRST, ATA spec §9.1) before probing - required because
      GRUB leaves the primary channel in a transient state after loading the kernel
- [x] ATA vs ATAPI classification via LBA1/LBA2 signature bytes
- [x] 28-bit LBA PIO read (`ide_read_sectors`)
- [x] 28-bit LBA PIO write (`ide_write_sectors`) + cache flush
- [x] `lsdisks` shell command - lists detected drives with type, size, model
- [x] `readsector <drive> <lba>` shell command - hex dump of one sector
- [x] Tested under QEMU with both IDE HDD (`-drive if=ide`) and CD-ROM (ATAPI)

## Source

- `src/kernel/arch/i386/drivers/ide.c`
- `src/kernel/include/kernel/ide.h`
- `docs/kernel/ide.md`
