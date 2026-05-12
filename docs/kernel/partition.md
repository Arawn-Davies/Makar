# Partition Driver (`partition.c` / `partition.h`)

## Overview

`partition.c` implements MBR and GPT partition table discovery and creation for
the Makar kernel.  It sits on top of the ATA/IDE driver (`ide.c`) and is used
by the kernel shell to list and interactively create partition tables.

Supported schemes:
- **MBR** - up to four primary 16-byte entries at offset `0x1BE` of sector 0.
- **GPT** - protective MBR in sector 0, primary header in sector 1, up to 128
  entries in sectors 2–33, with backup copies in the last 34 sectors of the
  disk.

No filesystem code is included - this layer handles only the partition table
metadata.

---

## Partition Schemes

| Constant           | Value | Description                    |
|--------------------|-------|--------------------------------|
| `PART_SCHEME_NONE` | 0     | No recognisable partition table |
| `PART_SCHEME_MBR`  | 1     | MBR partition table            |
| `PART_SCHEME_GPT`  | 2     | GUID Partition Table           |

---

## MBR Partition Types

Common type bytes recognised by `part_type_name()`:

| Constant              | Hex   | Description              |
|-----------------------|-------|--------------------------|
| `PART_MBR_EMPTY`      | `0x00`| Empty / unused           |
| `PART_MBR_FAT12`      | `0x01`| FAT12                    |
| `PART_MBR_FAT16_SM`   | `0x04`| FAT16 < 32 MiB           |
| `PART_MBR_FAT16`      | `0x06`| FAT16                    |
| `PART_MBR_NTFS`       | `0x07`| NTFS / exFAT             |
| `PART_MBR_FAT32_CHS`  | `0x0B`| FAT32 (CHS addressed)    |
| `PART_MBR_FAT32_LBA`  | `0x0C`| FAT32 (LBA addressed)    |
| `PART_MBR_LINUX_SWAP` | `0x82`| Linux swap               |
| `PART_MBR_LINUX`      | `0x83`| Linux native             |
| `PART_MBR_GPT_PROT`   | `0xEE`| GPT protective entry     |
| `PART_MBR_EFI`        | `0xEF`| EFI System Partition     |
| `PART_MBR_MDFS`       | `0xFA`| Makar / Medli File System|

`PART_MBR_MDFS` (`0xFA`) is the MBR type byte used by the Medli OS for its
native file system, mirroring the value chosen in
[`MFSU.cs`](https://github.com/Arawn-Davies/Medli/blob/main/Medli/Hardware/DiskUtility/MDFS.Physical/MFSU.cs).

---

## GPT Partition Type GUIDs

| Constant           | UUID string                            | Description           |
|--------------------|----------------------------------------|-----------------------|
| `PART_GUID_FAT32`  | `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7`| Microsoft Basic Data  |
| `PART_GUID_EFI`    | `C12A7328-F81F-11D2-BA4B-00A0C93EC93B`| EFI System Partition  |
| `PART_GUID_LINUX`  | `0FC63DAF-8483-4772-8E79-3D69D8477DE4`| Linux native data     |
| `PART_GUID_MDFS`   | `4D4B4452-5346-4200-8000-000000000001`| Makar/Medli FS (MDFS) |

GUIDs are stored in the on-disk mixed-endian format required by the UEFI spec
(first three fields little-endian, remaining eight bytes big-endian).

The MDFS GUID `4D4B4452-5346-4200-…` encodes `MKDR` + `SF` in its first six
bytes, identifying it as a Makar-native partition type.

---

## GPT Disk Layout

```
sector 0          protective MBR (single 0xEE entry covering the whole disk)
sector 1          primary GPT header (92 bytes, CRC32-signed, zero-padded to 512)
sectors 2–33      128 × 128-byte partition entries (16384 bytes)
sectors 34 – N-34 usable partition space
sectors N-33–N-2  backup partition entries  (N = total_sectors)
sector  N-1       backup GPT header
```

`part_write_gpt()` writes all six regions.  CRC32 checksums use the IEEE 802.3
polynomial (`0xEDB88320`) computed with a bit-by-bit loop (no static table).

---

## Data Structures

### `part_info_t`

```c
typedef struct {
    uint8_t  scheme;        /* PART_SCHEME_MBR or PART_SCHEME_GPT          */
    uint8_t  mbr_type;      /* MBR: partition type byte; GPT: 0            */
    uint8_t  bootable;      /* MBR: 1 if the bootable flag (0x80) is set   */
    uint8_t  _pad;
    uint32_t lba_start;     /* First sector (LBA)                          */
    uint32_t lba_count;     /* Number of sectors                           */
    char     name[37];      /* GPT: partition name (ASCII from UTF-16LE)   */
    uint8_t  type_guid[16]; /* GPT: partition type GUID (on-disk encoding) */
    uint8_t  part_guid[16]; /* GPT: unique partition GUID (on-disk encoding)*/
} part_info_t;
```

### `disk_parts_t`

```c
typedef struct {
    uint8_t     scheme;          /* PART_SCHEME_NONE / _MBR / _GPT        */
    uint32_t    total_sectors;   /* Total disk size in 512-byte sectors    */
    int         count;           /* Number of valid entries in parts[]     */
    part_info_t parts[128];
} disk_parts_t;
```

---

## Public API

### `int part_probe(uint8_t drive, disk_parts_t *out)`

Reads sector 0 of `drive` (0–3) and detects whether it contains an MBR or GPT
partition table.  If a protective MBR is found (`type == 0xEE`), it tries to
parse the GPT header at sector 1; falls back to MBR if the GPT is unreadable.

| Return | Meaning                                  |
|--------|------------------------------------------|
| `0`    | Success; check `out->scheme` for details |
| `-1`   | Drive index invalid or drive not present |
| other  | ATA I/O error from `ide_read_sectors()`  |

---

### `int part_write_mbr(uint8_t drive, const part_info_t *entries, int count)`

Reads sector 0, preserves the first 446 bytes (bootstrap code), then overwrites
the four 16-byte partition table slots and the `0x55AA` signature.

- `count` must be 1–4.
- Unused slots are zeroed.
- CHS fields are set to `0xFE 0xFF 0xFF` (beyond-CHS sentinel, LBA-only).

Returns 0 on success, non-zero on I/O error.

---

### `int part_write_gpt(uint8_t drive, const part_info_t *entries, int count)`

Writes a complete GPT structure: protective MBR, primary header, primary entry
array, backup entry array, backup header.

- `count` must be 0–128.
- Any entry whose `part_guid` is all zeros receives an auto-generated
  pseudo-random UUID (version 4, variant 1).
- Partition entries are written as 128-byte records in the standard layout.
- Both headers carry valid CRC32 checksums.

Returns 0 on success, non-zero on I/O error.

---

### `const char *part_type_name(uint8_t mbr_type)`

Maps an MBR partition type byte to a human-readable string.  Never returns
`NULL`.

---

### `const char *part_guid_type_name(const uint8_t *guid)`

Compares the 16-byte on-disk GUID against the known constants and returns a
string such as `"FAT32"`, `"EFI System"`, `"Linux Data"`, `"MDFS"`, `"Unused"`,
or `"Unknown"`.  Never returns `NULL`.

---

## Shell Commands

### `lspart <drive>`

Probes the drive and prints the partition table.  Handles both MBR and GPT
automatically.  Example MBR output:

```
Scheme: MBR  total sectors: 1048576  (512 MiB)
  [1] type=0x0C (FAT32 (LBA))  LBA=2048  sectors=524288  size=256 MiB
  [2] type=0xFA (MDFS)  LBA=526336  sectors=524288  size=256 MiB
```

Example GPT output:

```
Scheme: GPT  total sectors: 1048576  (512 MiB)
  [1] FAT32  "boot"  LBA=2048  sectors=204800  size=100 MiB
  [2] MDFS  "data"   LBA=206848  sectors=843776  size=412 MiB
```

### `mkpart <drive> <mbr|gpt>`

Interactive partition table creator.  Prompts for:
1. Number of partitions (1–4 for MBR, 1–128 for GPT).
2. For each partition: type keyword, size in MiB, and (GPT only) a name.

Supported type keywords: `fat32` `mdfs` `linux` `efi`

All partitions are 1 MiB aligned (start at a multiple of 2048 sectors).
After writing, the command re-reads and displays the new partition table.

---

## Implementation Notes

- All large sector buffers (`s_entry_buf` at 16384 bytes, etc.) are
  `static` - they live in BSS and never appear on the kernel stack.
- The CRC32 implementation is a bit-by-bit loop; no 1 KB static table is needed.
- Partition unique GUIDs are generated from `timer_get_ticks()` XOR'd with a
  running seed and per-entry discriminator.  The output is stamped with UUID
  version 4 and variant-1 bits.
- Only 28-bit LBA is used, matching the current IDE driver; drives larger than
  128 GiB are unsupported.
