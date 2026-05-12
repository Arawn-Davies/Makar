# FAT32 driver - read/write access

> **Status:** complete (read + write; HDD boot self-contained)
> **Branch:** landed on `main`

## Summary

FAT32 filesystem driver mounted at `/hd` from the first FAT32 partition
detected on any ATA drive.  ISO 9660 is also supported, mounted at `/cdrom`.
Both are unified under a VFS layer.

## Implemented

- [x] Mount FAT32 volume via BPB validation (`fat32_mount`)
- [x] Auto-mount on boot: `vfs_auto_mount()` scans all ATA drives, mounts
      first FAT32 partition at `/hd`; ISO 9660 mounted at `/cdrom`
- [x] Read files and directories (FAT32 cluster chain traversal)
- [x] Write files and create/delete directories
- [x] `ls`, `cd`, `cat`, `mkdir`, `rm`, `cp` shell commands
- [x] HDD image is self-contained - kernel at `/boot/makar.kernel`,
      userspace apps at `/apps/` (no CD-ROM required)
- [x] Path separator is `/` (POSIX-style, not `\`)

## Source

- `src/kernel/arch/i386/fs/fat32.c`
- `src/kernel/arch/i386/fs/iso9660.c`
- `src/kernel/arch/i386/fs/vfs.c`
