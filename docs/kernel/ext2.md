---
title: ext2
---

# ext2 filesystem driver

`src/kernel/arch/i386/fs/ext2.c` · `include/kernel/ext2.h`

A read **and** write ext2 driver, mirroring the FAT32 driver's public API so
the [VFS](../index.md) can dispatch the hard-disk mount table to either
backend. The intended layout is FAT32 for the kernel + bootloader modules +
root (EFI-partition style) and ext2 for apps / user directories.

## Supported on-disk formats

| Aspect | Support |
|---|---|
| Revision | rev 0 and rev 1 (dynamic) |
| Block size | 1024 / 2048 / 4096 bytes |
| Inode size | 128 (rev 0) or the superblock's `s_inode_size` (rev 1) |
| Incompat features | `FILETYPE` only — a volume with any other incompat feature (journal, extents, meta_bg, 64bit) is **refused** at mount so Makar never corrupts an ext3/ext4 volume |
| Block maps | direct (0–11) + single-indirect + double-indirect (triple-indirect is not implemented) |
| Directories | linear variable-length entries; `dir_index` (HTree) volumes should be created with `^dir_index` so the linear walk sees every entry |

## Public API

Mount/format:

- `ext2_probe(drive, lba)` — cheap superblock-magic test (`0xEF53`); no state change.
- `ext2_mount(drive, lba)` — `0`, `-2` (not ext2), `-3` (unsupported incompat), `-1` (I/O).
- `ext2_unmount()` — flush metadata + release the cached group-descriptor table.
- `ext2_mkfs(drive, lba, sectors)` — format as ext2 (1 KiB blocks, rev 1, FILETYPE): superblock + per-group descriptors / bitmaps / inode tables (super + GDT backup in every group), root directory and `lost+found`.

Operations (paths are volume-relative, **case-sensitive**, `/`-separated):

`ext2_ls` · `ext2_cd` · `ext2_mkdir` · `ext2_read_file` · `ext2_write_file` ·
`ext2_file_exists` · `ext2_delete_file` · `ext2_delete_dir` (non-empty refused) ·
`ext2_rename_file` / `ext2_rename_dir` · `ext2_complete` (tab completion).

## Write path

Block and inode allocation scan the per-group bitmaps for a free bit, set it,
decrement the group and superblock free counts, and flush the affected
metadata. `ext2_write_file` truncates an existing file (freeing its block map)
before rewriting, or allocates a fresh inode + directory entry. `ext2_mkdir`
seeds a new directory block with `.` / `..` and bumps the parent's link count.
Directory-entry insertion reuses slack in existing entries or appends a new
directory block.

## VFS integration

The VFS keeps a small mount table; each `/mnt/<name>` maps to a backend.
`mount /dev/hdaN /mnt/<name>` calls `vfs_mount_hd`, which prefers ext2 when a
superblock is present and falls back to FAT32. Because the FAT32 and ext2
drivers each hold a single volume, **one of each** can be mounted
simultaneously at different mountpoints. `mkfs.ext2 /dev/hdaN` formats a
partition; `umount /mnt/<name>` flushes and detaches it.

## Limitations / not yet done

- Single volume per backend (no two ext2 mounts at once).
- No triple-indirect blocks, no HTree (`dir_index`) traversal, no journalling.
- 128-byte inodes use pre-2038 timestamps (mkfs default).
- Clean-room implementation; not derived from the Linux ext2 source.
