/*
 * partition.c — MBR and GPT partition table driver.
 *
 * Provides:
 *   part_probe()       – detect partition scheme and enumerate entries
 *   part_write_mbr()   – write an MBR partition table
 *   part_write_gpt()   – write a complete GPT (protective MBR + headers +
 *                        primary/backup entry arrays)
 *   part_type_name()   – MBR type byte → printable string
 *   part_guid_type_name() – GPT type GUID → printable string
 *
 * GPT integrity uses CRC32 (IEEE 802.3 / zlib polynomial 0xEDB88320).
 * A bit-by-bit implementation avoids any BSS table overhead.
 *
 * All sector I/O is delegated to ide_read_sectors() / ide_write_sectors().
 * All sector buffers that would exceed reasonable stack depth are declared
 * as file-scope statics (they live in BSS, not on the stack).
 */

#include <kernel/partition.h>
#include <kernel/ide.h>
#include <kernel/timer.h>
#include <string.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Well-known GPT partition type GUIDs (on-disk mixed-endian encoding).
 *
 * The first three fields of a GUID are stored little-endian; the remaining
 * eight bytes are big-endian.  The values below match that encoding.
 *
 *   Field widths: 4B-LE | 2B-LE | 2B-LE | 8B-BE
 * ---------------------------------------------------------------------- */

/* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */
const uint8_t PART_GUID_FAT32[16] = {
    0xA2,0xA0,0xD0,0xEB, 0xE5,0xB9, 0x33,0x44,
    0x87,0xC0, 0x68,0xB6,0xB7,0x26,0x99,0xC7
};

/* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
const uint8_t PART_GUID_EFI[16] = {
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11,
    0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B
};

/* 0FC63DAF-8483-4772-8E79-3D69D8477DE4 */
const uint8_t PART_GUID_LINUX[16] = {
    0xAF,0x3D,0xC6,0x0F, 0x83,0x84, 0x72,0x47,
    0x8E,0x79, 0x3D,0x69,0xD8,0x47,0x7D,0xE4
};

/* 4D4B4452-5346-4200-8000-000000000001
 * "MKDR" + "SF" + 0x4200 + variant/version marker + serial 1
 * This is the Makar/Medli File System partition type GUID. */
const uint8_t PART_GUID_MDFS[16] = {
    0x52,0x44,0x4B,0x4D, 0x46,0x53, 0x00,0x42,
    0x80,0x00, 0x00,0x00,0x00,0x00,0x00,0x01
};

/* -------------------------------------------------------------------------
 * CRC32 (IEEE 802.3 / zlib, polynomial 0xEDB88320, bit-by-bit)
 * ---------------------------------------------------------------------- */
static uint32_t crc32_buf(const uint8_t *buf, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *buf++;
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* -------------------------------------------------------------------------
 * Little-endian read/write helpers
 * ---------------------------------------------------------------------- */
static uint32_t rd32(const uint8_t *buf, int off)
{
    return  (uint32_t)buf[off]
          | ((uint32_t)buf[off+1] <<  8)
          | ((uint32_t)buf[off+2] << 16)
          | ((uint32_t)buf[off+3] << 24);
}

/* Only the lower 32 bits of a 64-bit LE field (sufficient for <2 TiB). */
static uint32_t rd64lo(const uint8_t *buf, int off)
{
    return rd32(buf, off);
}

static void wr32(uint8_t *buf, int off, uint32_t v)
{
    buf[off]   = (uint8_t)(v);
    buf[off+1] = (uint8_t)(v >>  8);
    buf[off+2] = (uint8_t)(v >> 16);
    buf[off+3] = (uint8_t)(v >> 24);
}

/* Write a 64-bit LE value; upper 32 bits are always zero (LBA28 space). */
static void wr64(uint8_t *buf, int off, uint32_t v)
{
    wr32(buf, off,   v);
    wr32(buf, off+4, 0u);
}

/* -------------------------------------------------------------------------
 * Pseudo-random GUID generator
 *
 * Derives 16 bytes from timer_get_ticks() plus a caller-supplied
 * discriminator, then stamps UUID version 4 and variant-1 bits.
 * Not cryptographically strong, but gives unique GUIDs per session.
 * ---------------------------------------------------------------------- */
static uint32_t s_guid_seed;

static void guid_gen(uint8_t *guid, uint32_t discriminator)
{
    uint32_t a = timer_get_ticks() ^ discriminator ^ s_guid_seed ^ 0xDEADBEEFu;
    uint32_t b = a * 1664525u   + 1013904223u;
    uint32_t c = b * 22695477u  + 1u;
    uint32_t d = c * 1103515245u + 12345u;
    s_guid_seed = d;
    wr32(guid,  0, a);
    wr32(guid,  4, b);
    wr32(guid,  8, c);
    wr32(guid, 12, d);
    guid[6] = (guid[6] & 0x0Fu) | 0x40u; /* UUID version 4 */
    guid[8] = (guid[8] & 0x3Fu) | 0x80u; /* UUID variant 1 */
}

/* -------------------------------------------------------------------------
 * Partition type name tables
 * ---------------------------------------------------------------------- */
const char *part_type_name(uint8_t type)
{
    switch (type) {
    case PART_MBR_EMPTY:      return "Empty";
    case PART_MBR_FAT12:      return "FAT12";
    case PART_MBR_FAT16_SM:   return "FAT16 <32M";
    case PART_MBR_EXTENDED:   return "Extended (CHS)";
    case PART_MBR_FAT16:      return "FAT16";
    case PART_MBR_NTFS:       return "NTFS/exFAT";
    case PART_MBR_FAT32_CHS:  return "FAT32 (CHS)";
    case PART_MBR_FAT32_LBA:  return "FAT32 (LBA)";
    case PART_MBR_FAT16_LBA:  return "FAT16 (LBA)";
    case PART_MBR_EXT_LBA:    return "Extended (LBA)";
    case PART_MBR_LINUX_SWAP: return "Linux swap";
    case PART_MBR_LINUX:      return "Linux";
    case PART_MBR_LVM:        return "Linux LVM";
    case PART_MBR_GPT_PROT:   return "GPT protective";
    case PART_MBR_EFI:        return "EFI System";
    case PART_MBR_MDFS:       return "MDFS";
    default:                  return "Unknown";
    }
}

const char *part_guid_type_name(const uint8_t *guid)
{
    static const uint8_t zero[16] = {0};
    if (memcmp(guid, zero,           16) == 0) return "Unused";
    if (memcmp(guid, PART_GUID_FAT32, 16) == 0) return "FAT32";
    if (memcmp(guid, PART_GUID_EFI,   16) == 0) return "EFI System";
    if (memcmp(guid, PART_GUID_LINUX, 16) == 0) return "Linux Data";
    if (memcmp(guid, PART_GUID_MDFS,  16) == 0) return "MDFS";
    return "Unknown";
}

/* -------------------------------------------------------------------------
 * GPT entry buffer
 *
 * 128 entries × 128 bytes = 16384 bytes = 32 sectors.
 * Declared at file scope so it is never placed on the stack.
 * Shared (sequentially) by parse_gpt() and part_write_gpt().
 * ---------------------------------------------------------------------- */
static uint8_t s_entry_buf[128 * 128];

/* Convert a UTF-16LE GPT partition name (72 bytes, max 36 chars) to ASCII. */
static void gpt_name_to_ascii(const uint8_t *utf16le, char *out)
{
    for (int i = 0; i < 36; i++) {
        uint16_t c = (uint16_t)utf16le[i*2] | ((uint16_t)utf16le[i*2+1] << 8);
        if (c == 0u) { out[i] = '\0'; return; }
        out[i] = (c < 0x80u) ? (char)c : '?';
    }
    out[36] = '\0';
}

/* -------------------------------------------------------------------------
 * MBR parsing (internal)
 * ---------------------------------------------------------------------- */
static void parse_mbr(const uint8_t *sector0, disk_parts_t *out)
{
    out->scheme = PART_SCHEME_MBR;
    out->count  = 0;

    for (int i = 0; i < 4; i++) {
        int      base   = 0x1BE + i * 16;
        uint8_t  status = sector0[base];
        uint8_t  type   = sector0[base + 4];
        uint32_t start  = rd32(sector0, base +  8);
        uint32_t count  = rd32(sector0, base + 12);

        if (type == PART_MBR_EMPTY)
            continue;

        part_info_t *p   = &out->parts[out->count++];
        p->scheme        = PART_SCHEME_MBR;
        p->mbr_type      = type;
        p->bootable      = (status == 0x80u) ? 1u : 0u;
        p->_pad          = 0;
        p->lba_start     = start;
        p->lba_count     = count;
        p->name[0]       = '\0';
        memset(p->type_guid, 0, 16);
        memset(p->part_guid, 0, 16);
    }
}

/* -------------------------------------------------------------------------
 * GPT parsing (internal)
 * ---------------------------------------------------------------------- */
static int parse_gpt(uint8_t drive, const uint8_t *hdr_buf, disk_parts_t *out)
{
    static const uint8_t gpt_sig[8] = {'E','F','I',' ','P','A','R','T'};
    if (memcmp(hdr_buf, gpt_sig, 8) != 0)
        return -1;

    uint32_t num_entries   = rd32(hdr_buf, 80);
    uint32_t entry_size    = rd32(hdr_buf, 84);
    uint32_t entries_start = rd64lo(hdr_buf, 72);

    if (entry_size != 128u || num_entries > PART_MAX_ENTRIES)
        return -1;

    /* Read entry sectors (max 32 = 16384 bytes). */
    uint32_t entry_sectors = (num_entries * entry_size + 511u) / 512u;
    if (entry_sectors > 32u)
        entry_sectors = 32u;

    int err = ide_read_sectors(drive, entries_start,
                               (uint8_t)entry_sectors, s_entry_buf);
    if (err)
        return err;

    out->scheme = PART_SCHEME_GPT;
    out->count  = 0;

    for (uint32_t i = 0; i < num_entries && out->count < PART_MAX_ENTRIES; i++) {
        const uint8_t *e = s_entry_buf + i * entry_size;

        /* Skip unused entries (type GUID all zeros). */
        static const uint8_t zero[16] = {0};
        if (memcmp(e, zero, 16) == 0)
            continue;

        part_info_t *p  = &out->parts[out->count++];
        p->scheme       = PART_SCHEME_GPT;
        p->mbr_type     = 0;
        p->bootable     = 0;
        p->_pad         = 0;
        memcpy(p->type_guid, e,       16);
        memcpy(p->part_guid, e + 16,  16);

        /* first_lba and last_lba are 64-bit; we use the lower 32 bits. */
        uint32_t first = rd32(e, 32);
        uint32_t last  = rd32(e, 40);
        p->lba_start   = first;
        p->lba_count   = last - first + 1u;

        /* Parse UTF-16LE partition name (starts at byte 56 in the entry). */
        gpt_name_to_ascii(e + 56, p->name);
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * part_probe
 * ---------------------------------------------------------------------- */
int part_probe(uint8_t drive, disk_parts_t *out)
{
    static uint8_t s_sector0[512];
    static uint8_t s_gpt_hdr[512];

    const ide_drive_t *d = ide_get_drive(drive);
    if (!d || !d->present)
        return -1;

    out->scheme       = PART_SCHEME_NONE;
    out->total_sectors = d->size;
    out->count        = 0;

    int err = ide_read_sectors(drive, 0u, 1u, s_sector0);
    if (err)
        return err;

    /* Any valid partition table has the 0x55AA boot signature. */
    if (s_sector0[510] != 0x55u || s_sector0[511] != 0xAAu)
        return 0; /* no table — scheme stays NONE */

    /* If the first MBR entry has type 0xEE, this is a GPT protective MBR. */
    if (s_sector0[0x1BE + 4] == PART_MBR_GPT_PROT) {
        err = ide_read_sectors(drive, 1u, 1u, s_gpt_hdr);
        if (err == 0) {
            err = parse_gpt(drive, s_gpt_hdr, out);
            if (err == 0)
                return 0;
        }
        /* GPT unreadable — fall through and parse the protective MBR itself. */
    }

    parse_mbr(s_sector0, out);
    return 0;
}

/* -------------------------------------------------------------------------
 * part_write_mbr
 * ---------------------------------------------------------------------- */
int part_write_mbr(uint8_t drive, const part_info_t *entries, int count)
{
    static uint8_t s_mbr[512];

    if (count < 1 || count > 4)
        return -1;

    /* Preserve any boot code already in sector 0. */
    int err = ide_read_sectors(drive, 0u, 1u, s_mbr);
    if (err)
        return err;

    /* Zero the four partition table slots (0x1BE–0x1FD). */
    memset(s_mbr + 0x1BE, 0, 4 * 16);

    for (int i = 0; i < count; i++) {
        int base = 0x1BE + i * 16;

        s_mbr[base]     = entries[i].bootable ? 0x80u : 0x00u;
        /* CHS start: 0xFE 0xFF 0xFF marks "beyond CHS range, use LBA". */
        s_mbr[base + 1] = 0xFEu;
        s_mbr[base + 2] = 0xFFu;
        s_mbr[base + 3] = 0xFFu;
        s_mbr[base + 4] = entries[i].mbr_type;
        /* CHS end: same sentinel. */
        s_mbr[base + 5] = 0xFEu;
        s_mbr[base + 6] = 0xFFu;
        s_mbr[base + 7] = 0xFFu;
        wr32(s_mbr, base +  8, entries[i].lba_start);
        wr32(s_mbr, base + 12, entries[i].lba_count);
    }

    /* MBR boot signature. */
    s_mbr[510] = 0x55u;
    s_mbr[511] = 0xAAu;

    return ide_write_sectors(drive, 0u, 1u, s_mbr);
}

/* -------------------------------------------------------------------------
 * part_write_gpt
 *
 * Disk layout:
 *   sector 0              – protective MBR
 *   sector 1              – primary GPT header (92-byte, CRC32-signed)
 *   sectors 2–33          – 128 partition entries (16384 bytes / 32 sectors)
 *   sectors (N-33)–(N-2)  – backup partition entries (N = disk_sectors)
 *   sector  (N-1)         – backup GPT header
 *
 * First usable LBA:  34
 * Last  usable LBA:  disk_sectors − 34
 * ---------------------------------------------------------------------- */
int part_write_gpt(uint8_t drive, const part_info_t *entries, int count)
{
    static uint8_t s_pmbr[512];
    static uint8_t s_gpt_hdr[512];

    if (count < 0 || count > 128)
        return -1;

    const ide_drive_t *d = ide_get_drive(drive);
    if (!d || !d->present)
        return -1;

    uint32_t disk_sectors = d->size;
    if (disk_sectors < 68u) /* too small for GPT overhead */
        return -1;

    /* ---- protective MBR ---- */
    memset(s_pmbr, 0, sizeof(s_pmbr));
    /* Single entry covers the whole disk minus sector 0. */
    s_pmbr[0x1BE]     = 0x00u;                      /* not bootable */
    s_pmbr[0x1BE + 1] = 0x00u;                      /* CHS start (invalid) */
    s_pmbr[0x1BE + 2] = 0x02u;
    s_pmbr[0x1BE + 3] = 0x00u;
    s_pmbr[0x1BE + 4] = PART_MBR_GPT_PROT;          /* 0xEE */
    s_pmbr[0x1BE + 5] = 0xFFu;                      /* CHS end (beyond range) */
    s_pmbr[0x1BE + 6] = 0xFFu;
    s_pmbr[0x1BE + 7] = 0xFFu;
    wr32(s_pmbr, 0x1BE +  8, 1u);                   /* LBA start = 1 */
    /* LBA count: entire disk minus sector 0, capped at 0xFFFFFFFF. */
    uint32_t pmbr_count = (disk_sectors > 1u) ? disk_sectors - 1u : 0xFFFFFFFFu;
    wr32(s_pmbr, 0x1BE + 12, pmbr_count);
    s_pmbr[510] = 0x55u;
    s_pmbr[511] = 0xAAu;

    int err = ide_write_sectors(drive, 0u, 1u, s_pmbr);
    if (err) return err;

    /* ---- partition entries ---- */
    memset(s_entry_buf, 0, sizeof(s_entry_buf));
    s_guid_seed = timer_get_ticks();

    for (int i = 0; i < count; i++) {
        uint8_t *e = s_entry_buf + (uint32_t)i * 128u;

        memcpy(e,      entries[i].type_guid, 16);
        memcpy(e + 16, entries[i].part_guid, 16);

        /* Generate a unique partition GUID if the caller left it zeroed. */
        static const uint8_t zero16[16] = {0};
        if (memcmp(e + 16, zero16, 16) == 0)
            guid_gen(e + 16, (uint32_t)i * 0x1000u + (uint32_t)drive);

        uint32_t first = entries[i].lba_start;
        uint32_t last  = entries[i].lba_start + entries[i].lba_count - 1u;
        wr64(e, 32, first);       /* first LBA */
        wr64(e, 40, last);        /* last  LBA */
        wr64(e, 48, 0u);          /* attributes: none */

        /* Encode ASCII name as UTF-16LE (max 36 characters). */
        const char *name = entries[i].name;
        for (int j = 0; j < 36 && name[j] != '\0'; j++) {
            e[56 + j*2]     = (uint8_t)(uint8_t)name[j];
            e[56 + j*2 + 1] = 0u;
        }
    }

    uint32_t entries_crc = crc32_buf(s_entry_buf, 128u * 128u);

    /* Write primary entries: sectors 2–33. */
    err = ide_write_sectors(drive, 2u, 32u, s_entry_buf);
    if (err) return err;

    /* Write backup entries: sectors (disk_sectors−33) to (disk_sectors−2). */
    err = ide_write_sectors(drive, disk_sectors - 33u, 32u, s_entry_buf);
    if (err) return err;

    /* ---- generate disk GUID ---- */
    uint8_t disk_guid[16];
    guid_gen(disk_guid, (uint32_t)drive << 24);

    /* ---- primary GPT header (sector 1) ---- */
    memset(s_gpt_hdr, 0, sizeof(s_gpt_hdr));
    memcpy(s_gpt_hdr, "EFI PART", 8);            /*  0: signature           */
    wr32(s_gpt_hdr,  8, 0x00010000u);             /*  8: revision 1.0        */
    wr32(s_gpt_hdr, 12, 92u);                     /* 12: header size         */
    wr32(s_gpt_hdr, 16, 0u);                      /* 16: CRC32 (placeholder) */
    wr32(s_gpt_hdr, 20, 0u);                      /* 20: reserved            */
    wr64(s_gpt_hdr, 24, 1u);                      /* 24: my LBA              */
    wr64(s_gpt_hdr, 32, disk_sectors - 1u);       /* 32: backup LBA          */
    wr64(s_gpt_hdr, 40, 34u);                     /* 40: first usable LBA    */
    wr64(s_gpt_hdr, 48, disk_sectors - 34u);      /* 48: last usable LBA     */
    memcpy(s_gpt_hdr + 56, disk_guid, 16);        /* 56: disk GUID           */
    wr64(s_gpt_hdr, 72, 2u);                      /* 72: entry array LBA     */
    wr32(s_gpt_hdr, 80, 128u);                    /* 80: number of entries   */
    wr32(s_gpt_hdr, 84, 128u);                    /* 84: entry size          */
    wr32(s_gpt_hdr, 88, entries_crc);             /* 88: entries CRC32       */

    uint32_t hdr_crc = crc32_buf(s_gpt_hdr, 92u);
    wr32(s_gpt_hdr, 16, hdr_crc);

    err = ide_write_sectors(drive, 1u, 1u, s_gpt_hdr);
    if (err) return err;

    /* ---- backup GPT header (last sector) ---- */
    memset(s_gpt_hdr, 0, sizeof(s_gpt_hdr));
    memcpy(s_gpt_hdr, "EFI PART", 8);
    wr32(s_gpt_hdr,  8, 0x00010000u);
    wr32(s_gpt_hdr, 12, 92u);
    wr32(s_gpt_hdr, 16, 0u);                      /* CRC32 (placeholder) */
    wr32(s_gpt_hdr, 20, 0u);
    wr64(s_gpt_hdr, 24, disk_sectors - 1u);       /* my LBA = last sector    */
    wr64(s_gpt_hdr, 32, 1u);                      /* primary header LBA      */
    wr64(s_gpt_hdr, 40, 34u);
    wr64(s_gpt_hdr, 48, disk_sectors - 34u);
    memcpy(s_gpt_hdr + 56, disk_guid, 16);
    wr64(s_gpt_hdr, 72, disk_sectors - 33u);      /* backup entry array LBA  */
    wr32(s_gpt_hdr, 80, 128u);
    wr32(s_gpt_hdr, 84, 128u);
    wr32(s_gpt_hdr, 88, entries_crc);

    hdr_crc = crc32_buf(s_gpt_hdr, 92u);
    wr32(s_gpt_hdr, 16, hdr_crc);

    return ide_write_sectors(drive, disk_sectors - 1u, 1u, s_gpt_hdr);
}
