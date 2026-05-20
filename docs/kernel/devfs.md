---
title: devfs
parent: Kernel subsystems
---

# devfs - synthetic /dev block devices

`src/kernel/include/kernel/devfs.h` + `src/kernel/arch/i386/fs/devfs.c`.

## Purpose

A synthetic `/dev` mount that exposes raw IDE storage as byte-addressed
nodes. Unlike [procfs](procfs.md) (which renders text on demand), devfs nodes
are windows onto disk sectors: a read/write at any byte offset is translated
to the underlying sector I/O with internal read-modify-write.

## Nodes

The node table is (re)built by `devfs_init()` from the IDE scan (called at the
end of `vfs_init()`, and again after `fdisk` rewrites a partition table).

| Node | Backing | Access |
|---|---|---|
| `/dev/hda`, `/dev/hdb`, … | whole ATA disk (one letter per detected ATA drive) | read/write |
| `/dev/hda1` … `/dev/hda9` | that disk's MBR/GPT partitions (offset+length windows, via `part_probe`) | read/write |
| `/dev/cdrom` | the ATAPI optical drive | read-only |

ATA nodes use a 512-byte native sector; the CD-ROM uses 2048-byte ATAPI
sectors. Its size comes from `ide_atapi_capacity()` (READ CAPACITY(10)) since
ATAPI IDENTIFY carries no usable LBA range.

## API

| Function | Purpose |
|---|---|
| `devfs_init()` | Rescan the IDE bus and rebuild the node table. Idempotent. |
| `devfs_lookup(path)` | Resolve a devfs-relative path (`"/hda1"`) to a node index, or -1. |
| `devfs_file_exists(path)` | 1 if the path names a node. |
| `devfs_node_size(idx)` | Total addressable byte size of a node. |
| `devfs_node_readonly(idx)` | 1 for read-only nodes (CD-ROM). |
| `devfs_node_location(idx, *drive, *base_lba)` | Backing IDE drive + start LBA — lets `mount` translate `/dev/hdaN` into the (drive, lba) pair `fat32_mount` expects. |
| `devfs_pread(idx, buf, len, off)` / `devfs_pwrite(...)` | Byte-addressed I/O. Arbitrary offset/length via per-sector read-modify-write. Return bytes transferred or -1. |
| `devfs_ls(path)` / `devfs_complete(...)` | Directory listing + tab completion. |

## VFS integration

Wired in [vfs.c](vfs.md) as backend `VFS_FS_DEV`: `vfs_route()` recognises
`/dev`, and `ls` / `cat` / completion / existence all route through. Helpers
`vfs_blockdev_lookup()` / `vfs_blockdev_pread()` / `vfs_blockdev_pwrite()`
expose the node layer to the fd path.

## fd / syscall path

Opening a `/dev` node binds the fd as `FD_KIND_BLOCKDEV` (see
[kernel/fd.h]) **without** eager-buffering — a disk dwarfs `SYSCALL_FILE_MAX`.
The fd stores the devfs node index plus a byte cursor; `SYS_READ` /
`SYS_WRITE` / `SYS_LSEEK` route through `devfs_pread` / `devfs_pwrite` at the
fd's offset. Block-device fds carry no heap buffer, so the fd table's
clone/destroy paths need no special-casing.

## Consumers

- `fdisk.elf` opens `/dev/hda`, reads/writes the 512-byte MBR.
- `mount /dev/hda1 /mnt/<name>` resolves the node's geometry and mounts FAT32.
- A ktest suite (`test_devfs`) exercises lookup, the read-only flag, and a
  real `devfs_pread` of the boot CD's first sector.
