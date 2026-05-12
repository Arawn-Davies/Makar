#ifndef _KERNEL_PARTITION_H
#define _KERNEL_PARTITION_H

/*
 * partition.h - partition table discovery and management.
 *
 * Supports:
 *   MBR  - up to four primary partitions, 16-byte entries at sector 0.
 *   GPT  - up to 128 entries, with protective MBR, primary and backup headers.
 *
 * Partition type MDFS (0xFA / PART_GUID_MDFS) represents the Makar/Medli
 * File System used by both this OS and its sibling Medli.
 */

#include <kernel/types.h>

/* -------------------------------------------------------------------------
 * MBR partition type constants
 * ---------------------------------------------------------------------- */
#define PART_MBR_EMPTY      0x00
#define PART_MBR_FAT12      0x01
#define PART_MBR_FAT16_SM   0x04
#define PART_MBR_EXTENDED   0x05
#define PART_MBR_FAT16      0x06
#define PART_MBR_NTFS       0x07
#define PART_MBR_FAT32_CHS  0x0B
#define PART_MBR_FAT32_LBA  0x0C
#define PART_MBR_FAT16_LBA  0x0E
#define PART_MBR_EXT_LBA    0x0F
#define PART_MBR_LINUX_SWAP 0x82
#define PART_MBR_LINUX      0x83
#define PART_MBR_LVM        0x8E
#define PART_MBR_GPT_PROT   0xEE
#define PART_MBR_EFI        0xEF
#define PART_MBR_MDFS       0xFA  /* Medli/Makar File System */

/* -------------------------------------------------------------------------
 * Partition scheme identifiers
 * ---------------------------------------------------------------------- */
#define PART_SCHEME_NONE  0
#define PART_SCHEME_MBR   1
#define PART_SCHEME_GPT   2

/* Maximum number of partition entries we track per disk. */
#define PART_MAX_ENTRIES  128

/* -------------------------------------------------------------------------
 * Partition descriptor
 *
 * Both MBR and GPT partitions are described by this struct.
 * GPT-only fields (type_guid, part_guid, name) are zero/empty for MBR.
 * ---------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------------
 * Probed partition table for one disk
 * ---------------------------------------------------------------------- */
typedef struct {
    uint8_t     scheme;          /* PART_SCHEME_NONE / _MBR / _GPT        */
    uint32_t    total_sectors;   /* Total disk size in 512-byte sectors    */
    int         count;           /* Number of valid entries in parts[]     */
    part_info_t parts[PART_MAX_ENTRIES];
} disk_parts_t;

/* -------------------------------------------------------------------------
 * Well-known GPT partition type GUIDs (on-disk mixed-endian encoding).
 *
 * Stored in the mixed-endian byte order that GPT uses on disk:
 *   bytes 0-3:  time_low (32 bits, LE)
 *   bytes 4-5:  time_mid (16 bits, LE)
 *   bytes 6-7:  time_hi_version (16 bits, LE)
 *   bytes 8-15: clock_seq + node (big-endian)
 * ---------------------------------------------------------------------- */

/* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 (Microsoft Basic Data / FAT) */
extern const uint8_t PART_GUID_FAT32[16];

/* C12A7328-F81F-11D2-BA4B-00A0C93EC93B (EFI System Partition) */
extern const uint8_t PART_GUID_EFI[16];

/* 0FC63DAF-8483-4772-8E79-3D69D8477DE4 (Linux native filesystem data) */
extern const uint8_t PART_GUID_LINUX[16];

/* 4D4B4452-5346-4200-8000-000000000001 (Makar/Medli File System) */
extern const uint8_t PART_GUID_MDFS[16];

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/*
 * part_probe – detect and read the partition table on drive 0–3.
 *
 * Fills *out with scheme, total_sectors, count, and parts[].
 * Returns 0 on success (even if no partition table was found - check
 * out->scheme for PART_SCHEME_NONE in that case), or negative on I/O error.
 */
int part_probe(uint8_t drive, disk_parts_t *out);

/*
 * part_write_mbr – write a new MBR partition table.
 *
 * Reads sector 0, preserves the bootstrap code bytes 0x000–0x1BD, then
 * overwrites the four 16-byte partition entries (0x1BE–0x1FD) and the
 * 0x55AA signature.  'count' must be 1–4; excess slots are zeroed.
 * Returns 0 on success, non-zero on I/O error.
 */
int part_write_mbr(uint8_t drive, const part_info_t *entries, int count);

/*
 * part_write_gpt – write a complete new GPT.
 *
 * Writes:
 *   sector 0        – protective MBR
 *   sector 1        – primary GPT header (with CRC32)
 *   sectors 2–33    – 128 partition entries (16384 bytes)
 *   sectors n–n+31  – backup partition entries (n = disk_sectors − 33)
 *   last sector     – backup GPT header
 *
 * For any entry whose part_guid is all-zero the function generates a
 * pseudo-random UUID.  'count' must be 0–128.
 * Returns 0 on success, non-zero on I/O error.
 */
int part_write_gpt(uint8_t drive, const part_info_t *entries, int count);

/*
 * part_type_name – human-readable string for an MBR type byte.
 * Returns a pointer to a string literal; never NULL.
 */
const char *part_type_name(uint8_t mbr_type);

/*
 * part_guid_type_name – human-readable string for a GPT type GUID.
 * 'guid' is 16 bytes in on-disk (mixed-endian) format.
 * Returns a pointer to a string literal; never NULL.
 */
const char *part_guid_type_name(const uint8_t *guid);

#endif /* _KERNEL_PARTITION_H */
