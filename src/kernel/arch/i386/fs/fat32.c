/*
 * fat32.c - FAT32 read/write filesystem driver.
 *
 * Supports one mounted volume at a time.  All paths accept both '\' and '/'
 * as separators and are matched case-insensitively.
 *
 * Read features:
 *   - BPB parsing, cluster-chain walking
 *   - Directory listing with LFN (long file name) display
 *   - File read via fat32_read_file()
 *   - Path navigation and cd
 *
 * Write features:
 *   - File create / overwrite with LFN entries for long names; 8.3 basis
 *     name (XXXXXX~1.EXT) written for compatibility
 *   - Directory create with . and .. entries
 *   - FAT update with FAT2 mirroring
 *   - FAT32 format via fat32_mkfs()
 *
 * All sector I/O delegates to ide_read_sectors() / ide_write_sectors().
 * Large buffers are declared at file scope to keep the kernel stack safe.
 */

#include <kernel/fat32.h>
#include <kernel/ide.h>
#include <kernel/tty.h>
#include <string.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * FAT32 constants
 * ---------------------------------------------------------------------- */
#define FAT32_EOC         0x0FFFFFF8u   /* end-of-chain threshold */
#define FAT32_BAD         0x0FFFFFF7u   /* bad-cluster marker */
#define FAT32_FREE        0x00000000u   /* free cluster */

#define ATTR_RDONLY  0x01u
#define ATTR_HIDDEN  0x02u
#define ATTR_SYSTEM  0x04u
#define ATTR_VOLID   0x08u
#define ATTR_DIR     0x10u
#define ATTR_ARCH    0x20u
#define ATTR_LFN     0x0Fu  /* all four low attribute bits set */

#define DIRSCAN_LIST     0  /* list all entries to the terminal */
#define DIRSCAN_FIND     1  /* find a named entry */
#define DIRSCAN_COMPLETE 2  /* collect entries matching a prefix into a callback */

/* -------------------------------------------------------------------------
 * Volume state
 * ---------------------------------------------------------------------- */
typedef struct {
    uint8_t  mounted;
    uint8_t  drive;
    uint32_t part_lba;
    uint8_t  spc;           /* sectors per cluster           */
    uint32_t rsvd;          /* reserved sectors              */
    uint8_t  nfats;         /* number of FATs                */
    uint32_t spf;           /* sectors per FAT               */
    uint32_t root_cluster;
    uint32_t fat_lba;       /* LBA of FAT1                   */
    uint32_t data_lba;      /* LBA of data region            */
    uint32_t total_clusters;
    uint32_t free_hint;     /* cluster search start hint     */
    uint32_t cwd_cluster;   /* cluster of working directory  */
    char     cwd_path[256]; /* printable working-directory   */
} fat32_vol_t;

static fat32_vol_t vol;

/* -------------------------------------------------------------------------
 * Static sector buffers - never on the kernel stack
 * ---------------------------------------------------------------------- */
static uint8_t  s_sec[512];        /* general-purpose sector scratch   */
static uint8_t  s_fat[512];        /* FAT sector cache                 */
static uint32_t s_fat_lba;         /* which sector is cached           */
static int      s_fat_dirty;       /* cache needs writing               */

/* LFN assembly buffer: up to 20 LFN entries × 13 chars + NUL */
static char s_lfn[261];
static int  s_lfn_valid;

/* -------------------------------------------------------------------------
 * Internal parsed directory entry
 * ---------------------------------------------------------------------- */
typedef struct {
    char     name[256];     /* NUL-terminated filename (LFN or 8.3)      */
    uint8_t  attr;          /* FAT attribute byte                         */
    uint32_t first_cluster; /* first data cluster                         */
    uint32_t file_size;     /* bytes (0 for directories)                  */
    uint32_t ent_lba;       /* LBA of the sector containing the 8.3 entry */
    uint32_t ent_off;       /* byte offset within that sector             */
} dirent_t;

/* -------------------------------------------------------------------------
 * Little-endian helpers
 * ---------------------------------------------------------------------- */
static uint16_t rd16(const uint8_t *b, int o)
{
    return (uint16_t)b[o] | ((uint16_t)b[o+1] << 8);
}

static uint32_t rd32(const uint8_t *b, int o)
{
    return  (uint32_t)b[o]
          | ((uint32_t)b[o+1] <<  8)
          | ((uint32_t)b[o+2] << 16)
          | ((uint32_t)b[o+3] << 24);
}

static void wr16(uint8_t *b, int o, uint16_t v)
{
    b[o]   = (uint8_t)v;
    b[o+1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *b, int o, uint32_t v)
{
    b[o]   = (uint8_t)v;
    b[o+1] = (uint8_t)(v >>  8);
    b[o+2] = (uint8_t)(v >> 16);
    b[o+3] = (uint8_t)(v >> 24);
}

/* -------------------------------------------------------------------------
 * Case-insensitive string compare (no strcasecmp in kernel libc)
 * ---------------------------------------------------------------------- */
static int fat_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'a' && ca <= 'z') ca = (unsigned char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (unsigned char)(cb - 32);
        if (ca != cb) return (int)ca - (int)cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* -------------------------------------------------------------------------
 * Path separator predicate
 * ---------------------------------------------------------------------- */
static int is_sep(char c) { return c == '\\' || c == '/'; }

/* -------------------------------------------------------------------------
 * FAT sector cache
 *
 * fat_writeback_cached() flushes s_fat to FAT1 (and FAT2 if present).
 * fat_read() / fat_write() load/modify the cached FAT sector on demand.
 * ---------------------------------------------------------------------- */

static int fat_writeback_cached(void)
{
    if (!s_fat_dirty || s_fat_lba == 0xFFFFFFFFu)
        return 0;

    if (ide_write_sectors(vol.drive, s_fat_lba, 1, s_fat))
        return -1;

    /* Mirror write to FAT2. */
    if (vol.nfats > 1)
        ide_write_sectors(vol.drive, s_fat_lba + vol.spf, 1, s_fat);

    s_fat_dirty = 0;
    return 0;
}

/* Read the FAT entry for cluster; returns FAT32_EOC on error. */
static uint32_t fat_read(uint32_t cluster)
{
    if (cluster < 2 || cluster >= vol.total_clusters + 2u)
        return FAT32_EOC;

    uint32_t byte_off  = cluster * 4u;
    uint32_t fat_sect  = vol.fat_lba + byte_off / 512u;
    uint32_t ent_off   = byte_off % 512u;

    if (s_fat_lba != fat_sect) {
        if (fat_writeback_cached())
            return FAT32_EOC;
        if (ide_read_sectors(vol.drive, fat_sect, 1, s_fat))
            return FAT32_EOC;
        s_fat_lba = fat_sect;
    }

    return rd32(s_fat, (int)ent_off) & 0x0FFFFFFFu;
}

/* Write value into FAT entry for cluster; returns 0 on success. */
static int fat_write(uint32_t cluster, uint32_t value)
{
    if (cluster < 2)
        return -1;

    uint32_t byte_off = cluster * 4u;
    uint32_t fat_sect = vol.fat_lba + byte_off / 512u;
    uint32_t ent_off  = byte_off % 512u;

    if (s_fat_lba != fat_sect) {
        if (fat_writeback_cached())
            return -1;
        if (ide_read_sectors(vol.drive, fat_sect, 1, s_fat))
            return -1;
        s_fat_lba = fat_sect;
    }

    /* Preserve the top 4 bits (reserved by FAT32 spec). */
    uint8_t top = s_fat[ent_off + 3] & 0xF0u;
    wr32(s_fat, (int)ent_off, value & 0x0FFFFFFFu);
    s_fat[ent_off + 3] = (s_fat[ent_off + 3] & 0x0Fu) | top;
    s_fat_dirty = 1;
    return 0;
}

/* Flush any dirty FAT cache. */
static int fat_flush(void)
{
    return fat_writeback_cached();
}

/* Allocate one free cluster, link it from prev_cluster (if >= 2).
 * Returns the new cluster number, or 0 on failure. */
static uint32_t fat_alloc(uint32_t prev_cluster)
{
    uint32_t limit = vol.total_clusters + 2u;

    /* Search from hint, then wrap around from 2. */
    for (uint32_t pass = 0; pass < 2u; pass++) {
        uint32_t start = (pass == 0) ? vol.free_hint : 2u;
        uint32_t end   = (pass == 0) ? limit         : vol.free_hint;

        for (uint32_t c = start; c < end; c++) {
            if (fat_read(c) == FAT32_FREE) {
                if (fat_write(c, 0x0FFFFFFFu))  /* mark EOC */
                    return 0;
                if (prev_cluster >= 2u)
                    fat_write(prev_cluster, c);  /* link chain */
                vol.free_hint = c + 1u;
                if (vol.free_hint >= limit)
                    vol.free_hint = 2u;
                return c;
            }
        }
    }
    return 0; /* disk full */
}

/* Free an entire cluster chain starting at cluster. */
static void fat_free_chain(uint32_t cluster)
{
    while (cluster >= 2u && cluster < FAT32_BAD) {
        uint32_t next = fat_read(cluster);
        fat_write(cluster, FAT32_FREE);
        if (cluster < vol.free_hint)
            vol.free_hint = cluster;
        cluster = next;
    }
}

/* -------------------------------------------------------------------------
 * Cluster ↔ LBA
 * ---------------------------------------------------------------------- */
static uint32_t clus_to_lba(uint32_t cluster)
{
    return vol.data_lba + (cluster - 2u) * (uint32_t)vol.spc;
}

/* -------------------------------------------------------------------------
 * Generate a FAT 8.3 name (11 bytes, space-padded, uppercase).
 * ---------------------------------------------------------------------- */
static void make_83_name(const char *src, uint8_t *dst)
{
    memset(dst, ' ', 11);

    /* Find the last dot for the extension. */
    int dot = -1;
    for (int i = 0; src[i]; i++)
        if (src[i] == '.') dot = i;

    /* Fill 8-char name. */
    int n = 0;
    for (int i = 0; src[i] && (dot < 0 || i < dot) && n < 8; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        dst[n++] = c;
    }

    /* Fill 3-char extension. */
    if (dot >= 0) {
        int e = 0;
        for (int i = dot + 1; src[i] && e < 3; i++) {
            unsigned char c = (unsigned char)src[i];
            if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
            dst[8 + e++] = c;
        }
    }
}

/* -------------------------------------------------------------------------
 * LFN helpers
 * ---------------------------------------------------------------------- */

/* LFN offsets of the 13 UTF-16LE characters within one LFN entry. */
static const int s_lfn_char_off[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };

/* Forward declaration - dir_add_entry is defined after the LFN block. */
static uint32_t dir_add_entry(uint32_t dir_cluster, const uint8_t *short_name,
                               uint8_t attr, uint32_t first_cluster,
                               uint32_t file_size, uint32_t *out_off);

/*
 * Maximum LFN entries per file (FAT32 spec allows 20; 20 × 13 = 260 chars).
 */
#define MAX_LFN_ENTRIES 20

/*
 * lfn_needs_lfn – return 1 if 'name' cannot be stored losslessly as a plain
 * 8.3 entry (base > 8 chars, extension > 3 chars, or contains lowercase).
 */
static int lfn_needs_lfn(const char *name)
{
    int dot = -1, len = 0;
    for (; name[len]; len++)
        if (name[len] == '.') dot = len;

    int base_len = (dot >= 0) ? dot        : len;
    int ext_len  = (dot >= 0) ? len - dot - 1 : 0;

    if (base_len > 8 || ext_len > 3)
        return 1;

    for (int i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c >= 'a' && c <= 'z')
            return 1;           /* lowercase can't round-trip through 8.3 */
        if (name[i] == '.' && i != dot)
            return 1;           /* multiple dots */
    }
    return 0;
}

/*
 * make_basis_name – generate the 8.3 "basis name" (numeric-tail form
 * XXXXXX~1 + EXT, uppercase, space-padded) used as the short-name entry
 * when LFN entries are written.
 *
 * dst must point to an 11-byte buffer (no NUL).
 */
static void make_basis_name(const char *src, uint8_t *dst)
{
    memset(dst, ' ', 11);

    int dot = -1, len = 0;
    for (; src[len]; len++)
        if (src[len] == '.') dot = len;

    /* Up to 6 chars of the basename, then "~1". */
    int n = 0;
    for (int i = 0; src[i] && (dot < 0 || i < dot) && n < 6; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32u);
        dst[n++] = c;
    }
    if (n < 8) dst[n++] = '~';
    if (n < 8) dst[n++] = '1';

    /* Up to 3 chars of the extension. */
    if (dot >= 0) {
        int e = 0;
        for (int i = dot + 1; src[i] && e < 3; i++) {
            unsigned char c = (unsigned char)src[i];
            if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32u);
            dst[8 + e++] = c;
        }
    }
}

/*
 * lfn_checksum – compute the 8-bit checksum of an 11-byte 8.3 directory-
 * entry name as defined by the FAT32 LFN specification (MS-DOS 7 §13).
 * Each step right-rotates the running sum by one bit, then adds the next
 * name byte.
 */
static uint8_t lfn_checksum(const uint8_t *basis)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + basis[i]);
    return sum;
}

/*
 * dir_add_with_lfn – add a directory entry, prepending LFN entries when the
 * name cannot fit losslessly in 8.3 format.
 *
 * When LFN is not needed the function is identical to dir_add_entry().
 * When LFN IS needed it:
 *   1. Generates an 8.3 basis name (XXXXXX~1.EXT).
 *   2. Scans the directory for (n_lfn + 1) consecutive free slots.
 *   3. Writes the LFN entries in reverse on-disk order (last chars first,
 *      first entry marked with 0x40), followed by the 8.3 entry.
 *
 * Returns the LBA of the sector that contains the 8.3 entry, 0 on error.
 * *out_off receives the byte offset of the 8.3 entry within that sector.
 */
static uint32_t dir_add_with_lfn(uint32_t       dir_cluster,
                                  const char    *long_name,
                                  uint8_t        attr,
                                  uint32_t       first_cluster,
                                  uint32_t       file_size,
                                  uint32_t      *out_off)
{
    /* Fast path: name fits in 8.3, no LFN needed. */
    if (!lfn_needs_lfn(long_name)) {
        uint8_t sname[11];
        make_83_name(long_name, sname);
        return dir_add_entry(dir_cluster, sname, attr,
                             first_cluster, file_size, out_off);
    }

    int name_len = (int)strlen(long_name);
    int n_lfn    = (name_len + 12) / 13;   /* ceil(name_len / 13) */
    int n_needed = n_lfn + 1;              /* LFN entries + 8.3 entry */

    if (n_lfn > MAX_LFN_ENTRIES)
        return 0;   /* name too long for FAT32 */

    uint8_t basis[11];
    make_basis_name(long_name, basis);
    uint8_t cksum = lfn_checksum(basis);

    /*
     * Scan the directory cluster chain for a run of n_needed consecutive
     * free slots (first byte 0x00 = end-of-dir, or 0xE5 = deleted).
     * Record the (lba, slot-index) of each slot in the run.
     */
    uint32_t slot_lba[MAX_LFN_ENTRIES + 1];
    int      slot_idx[MAX_LFN_ENTRIES + 1];
    int      run_len = 0;
    int      found   = 0;

    for (uint32_t cluster = dir_cluster;
         !found && cluster >= 2u && cluster < FAT32_BAD; )
    {
        uint32_t base_lba = clus_to_lba(cluster);

        for (uint8_t s = 0; s < vol.spc && !found; s++) {
            uint32_t lba = base_lba + (uint32_t)s;
            if (ide_read_sectors(vol.drive, lba, 1, s_sec))
                return 0;

            for (int i = 0; i < 16 && !found; i++) {
                uint8_t fb = s_sec[i * 32];
                if (fb == 0x00u || fb == 0xE5u) {
                    slot_lba[run_len] = lba;
                    slot_idx[run_len] = i;
                    run_len++;
                    if (run_len >= n_needed)
                        found = 1;
                } else {
                    run_len = 0;   /* non-free entry breaks the run */
                }
            }
        }

        if (found) break;

        uint32_t next = fat_read(cluster);
        if (next >= FAT32_BAD) {
            /* Chain exhausted: allocate and zero a new cluster. */
            uint32_t nc = fat_alloc(cluster);
            if (!nc) return 0;
            if (fat_flush()) return 0;

            uint32_t new_lba = clus_to_lba(nc);
            memset(s_sec, 0, 512);
            for (uint8_t s2 = 0; s2 < vol.spc; s2++) {
                if (ide_write_sectors(vol.drive, new_lba + s2, 1, s_sec))
                    return 0;
            }

            /* All slots in the new cluster are free (0x00). */
            run_len = 0;
            int total_slots = (int)vol.spc * 16;
            for (int i = 0; i < n_needed && i < total_slots; i++) {
                slot_lba[run_len] = new_lba + (uint32_t)(i / 16);
                slot_idx[run_len] = i % 16;
                run_len++;
            }
            if (run_len >= n_needed)
                found = 1;
            break;
        }
        cluster = next;
    }

    if (!found)
        return 0;

    /*
     * Write LFN entries.  On-disk order is: last-chars entry first
     * (sequence n_lfn, flagged 0x40), down to sequence 1, then the 8.3.
     *
     * slot[0]         → LFN entry covering chars [(n_lfn-1)*13 .. end]
     * slot[n_lfn - 1] → LFN entry covering chars [0 .. 12]
     * slot[n_lfn]     → 8.3 entry
     */
    for (int k = 0; k < n_lfn; k++) {
        int     seq      = n_lfn - k;   /* n_lfn, n_lfn-1, …, 1 */
        uint8_t ord      = (uint8_t)seq;
        if (k == 0)
            ord |= 0x40u;               /* first on disk = last in name */

        int char_base = (seq - 1) * 13; /* name[char_base .. char_base+12] */

        uint32_t lba = slot_lba[k];
        if (ide_read_sectors(vol.drive, lba, 1, s_sec))
            return 0;

        uint8_t *e = s_sec + slot_idx[k] * 32;
        memset(e, 0, 32);
        e[0]  = ord;
        e[11] = ATTR_LFN;
        e[12] = 0;       /* type */
        e[13] = cksum;
        /* bytes 26-27: cluster must be 0 in LFN entries */

        for (int j = 0; j < 13; j++) {
            int      ci  = char_base + j;
            /*
             * Encode as UTF-16LE.  long_name is installer-supplied ASCII,
             * so the high byte is always 0 and the cast is safe.
             */
            uint16_t ucs = (ci < name_len)    ? (uint16_t)(unsigned char)long_name[ci]
                         : (ci == name_len)   ? 0x0000u   /* NUL terminator */
                         :                      0xFFFFu;  /* padding */
            e[s_lfn_char_off[j]]     = (uint8_t)(ucs & 0xFFu);
            e[s_lfn_char_off[j] + 1] = (uint8_t)(ucs >> 8);
        }

        if (ide_write_sectors(vol.drive, lba, 1, s_sec))
            return 0;
    }

    /* Write the 8.3 entry in the final slot. */
    {
        int      k   = n_lfn;
        uint32_t lba = slot_lba[k];
        if (ide_read_sectors(vol.drive, lba, 1, s_sec))
            return 0;

        uint8_t *e = s_sec + slot_idx[k] * 32;
        memset(e, 0, 32);
        memcpy(e, basis, 11);
        e[11] = attr;
        wr16(e, 20, (uint16_t)(first_cluster >> 16));
        wr16(e, 26, (uint16_t)(first_cluster & 0xFFFFu));
        wr32(e, 28, file_size);

        if (ide_write_sectors(vol.drive, lba, 1, s_sec))
            return 0;
        if (out_off)
            *out_off = (uint32_t)(slot_idx[k] * 32u);
        return lba;
    }
}

/* -------------------------------------------------------------------------
 * Directory scanner
 *
 * Walks every sector in the cluster chain of dir_cluster.
 *
 * DIRSCAN_LIST: prints every valid entry to the terminal; returns 0.
 * DIRSCAN_FIND: searches for find_name; fills *out and returns 0 if found,
 *               -1 if not found, -2 on I/O error.
 *
 * LFN entries are accumulated in s_lfn / s_lfn_valid across iterations.
 * The accumulation is reset at the start of each call.
 * ---------------------------------------------------------------------- */

static int dir_scan(uint32_t dir_cluster, int mode,
                    const char *find_name, dirent_t *out,
                    fat32_complete_cb_t cb, void *cb_ctx)
{
    s_lfn_valid = 0;
    int end_of_dir = 0;

    for (uint32_t cluster = dir_cluster;
         !end_of_dir && cluster >= 2u && cluster < FAT32_BAD;
         cluster = fat_read(cluster))
    {
        uint32_t base_lba = clus_to_lba(cluster);

        for (uint8_t s = 0; s < vol.spc && !end_of_dir; s++) {
            uint32_t lba = base_lba + s;

            if (ide_read_sectors(vol.drive, lba, 1, s_sec))
                return -2;

            for (int i = 0; i < 16 && !end_of_dir; i++) {
                uint8_t *e = s_sec + i * 32;

                /* 0x00: no more entries in this directory. */
                if (e[0] == 0x00) { end_of_dir = 1; break; }

                /* 0xE5: deleted entry. */
                if ((uint8_t)e[0] == 0xE5u) { s_lfn_valid = 0; continue; }

                uint8_t attr = e[11];

                /* LFN entry: accumulate characters. */
                if ((attr & 0x3Fu) == ATTR_LFN) {
                    int seq = e[0] & 0x1F;
                    if (seq < 1 || seq > 20) { s_lfn_valid = 0; continue; }
                    if (e[0] & 0x40u) {
                        /* First on disk = last in name: clear buffer. */
                        memset(s_lfn, 0, sizeof(s_lfn));
                    }
                    int base = (seq - 1) * 13;
                    for (int j = 0; j < 13 && base + j < 260; j++) {
                        uint16_t c = (uint16_t)e[s_lfn_char_off[j]]
                                   | ((uint16_t)e[s_lfn_char_off[j]+1] << 8);
                        s_lfn[base + j] = (c == 0u)    ? '\0'
                                        : (c  < 0x80u) ? (char)c
                                        :                '?';
                    }
                    s_lfn_valid = 1;
                    continue;
                }

                /* Volume label: skip. */
                if (attr & ATTR_VOLID) { s_lfn_valid = 0; continue; }

                /* Regular 8.3 entry ---------------------------------------- */

                /* Build the 8.3 display name. */
                char short_name[13];
                int  n = 0;
                for (int j = 0; j < 8 && e[j] != ' '; j++)
                    short_name[n++] = (char)e[j];
                if (e[8] != ' ') {
                    short_name[n++] = '.';
                    for (int j = 8; j < 11 && e[j] != ' '; j++)
                        short_name[n++] = (char)e[j];
                }
                short_name[n] = '\0';

                /* Prefer LFN if we have one. */
                const char *disp = (s_lfn_valid && s_lfn[0] != '\0')
                                   ? s_lfn : short_name;
                s_lfn_valid = 0;

                uint32_t fc = ((uint32_t)rd16(e, 20) << 16) | rd16(e, 26);
                uint32_t sz = rd32(e, 28);

                if (mode == DIRSCAN_LIST) {
                    if (attr & ATTR_DIR) t_putchar('[');
                    t_writestring(disp);
                    if (attr & ATTR_DIR) {
                        t_putchar(']');
                    } else {
                        t_writestring("  (");
                        t_dec(sz);
                        t_writestring(" B)");
                    }
                    t_putchar('\n');
                } else if (mode == DIRSCAN_COMPLETE) {
                    if (cb) cb(disp, (attr & ATTR_DIR) ? 1 : 0, cb_ctx);
                } else {
                    /* Match either the long name or the short name. */
                    if (fat_strcasecmp(disp, find_name) == 0 ||
                        fat_strcasecmp(short_name, find_name) == 0)
                    {
                        size_t dlen = strlen(disp);
                        if (dlen > 255u) dlen = 255u;
                        memcpy(out->name, disp, dlen + 1u);
                        out->attr          = attr;
                        out->first_cluster = fc;
                        out->file_size     = sz;
                        out->ent_lba       = lba;
                        out->ent_off       = (uint32_t)(i * 32);
                        return 0;
                    }
                }
            }
        }
    }

    return (mode == DIRSCAN_LIST) ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * Path helpers
 * ---------------------------------------------------------------------- */

/*
 * path_resolve_dir – resolve a path string to a directory cluster.
 * Returns 0 on error (cluster 0 is never valid in FAT32 data region).
 */
static uint32_t path_resolve_dir(const char *path)
{
    uint32_t    cluster;
    const char *p;

    if (!path || !*path)
        return vol.cwd_cluster;

    if (is_sep(path[0])) {
        cluster = vol.root_cluster;
        p = path + 1;
    } else {
        cluster = vol.cwd_cluster;
        p = path;
    }

    while (*p) {
        /* Extract one path component. */
        char comp[256];
        int  len = 0;
        while (*p && !is_sep(*p)) {
            if (len < 255) comp[len++] = *p;
            p++;
        }
        comp[len] = '\0';
        if (*p) p++; /* skip separator */
        if (len == 0) continue;
        if (strcmp(comp, ".") == 0) continue;

        /* Look up component in current cluster. */
        dirent_t ent;
        if (dir_scan(cluster, DIRSCAN_FIND, comp, &ent, NULL, NULL) != 0)
            return 0;
        if (!(ent.attr & ATTR_DIR))
            return 0;

        cluster = ent.first_cluster ? ent.first_cluster : vol.root_cluster;
    }

    return cluster;
}

/*
 * path_split – split a path into a parent-directory cluster and the
 * final component (basename).  Returns 0 on success.
 */
static int path_split(const char *path,
                      uint32_t   *parent_cluster,
                      const char **basename)
{
    if (!path || !*path) return -1;

    /* Find last separator. */
    const char *last_sep = NULL;
    for (const char *q = path; *q; q++)
        if (is_sep(*q)) last_sep = q;

    if (!last_sep) {
        *parent_cluster = vol.cwd_cluster;
        *basename       = path;
        return 0;
    }

    if (last_sep == path) {
        /* "/foo" - parent is root. */
        *parent_cluster = vol.root_cluster;
        *basename       = last_sep + 1;
        return 0;
    }

    /* General case: resolve everything before the last separator. */
    char parent_path[256];
    int  plen = (int)(last_sep - path);
    if (plen > 255) plen = 255;
    memcpy(parent_path, path, (size_t)plen);
    parent_path[plen] = '\0';

    uint32_t pc = path_resolve_dir(parent_path);
    if (pc == 0) return -1;
    *parent_cluster = pc;
    *basename       = last_sep + 1;
    return 0;
}

/* -------------------------------------------------------------------------
 * dir_add_entry – append a new 8.3 entry to a directory.
 *
 * Scans the directory cluster chain for a free slot (0x00 or 0xE5 first
 * byte).  If none is found a new cluster is allocated.
 *
 * Returns the LBA of the sector that received the entry, or 0 on error.
 * *out_off is set to the byte offset of the entry within that sector.
 * ---------------------------------------------------------------------- */
static uint32_t dir_add_entry(uint32_t        dir_cluster,
                              const uint8_t  *short_name,
                              uint8_t         attr,
                              uint32_t        first_cluster,
                              uint32_t        file_size,
                              uint32_t       *out_off)
{
    for (uint32_t cluster = dir_cluster;
         cluster >= 2u && cluster < FAT32_BAD;
         )
    {
        uint32_t lba = clus_to_lba(cluster);

        for (uint8_t s = 0; s < vol.spc; s++) {
            uint32_t sect = lba + s;
            if (ide_read_sectors(vol.drive, sect, 1, s_sec))
                return 0;

            for (int i = 0; i < 16; i++) {
                uint8_t first_byte = s_sec[i * 32];
                if (first_byte == 0x00u || (uint8_t)first_byte == 0xE5u) {
                    /* Free slot: write the entry. */
                    uint8_t *e = s_sec + i * 32;
                    memset(e, 0, 32);
                    memcpy(e, short_name, 11);
                    e[11] = attr;
                    wr16(e, 20, (uint16_t)(first_cluster >> 16));
                    wr16(e, 26, (uint16_t)(first_cluster));
                    wr32(e, 28, file_size);

                    if (ide_write_sectors(vol.drive, sect, 1, s_sec))
                        return 0;
                    if (out_off) *out_off = (uint32_t)(i * 32);
                    return sect;
                }
            }
        }

        uint32_t next = fat_read(cluster);
        if (next >= FAT32_BAD) {
            /* Chain exhausted - allocate a new cluster. */
            uint32_t nc = fat_alloc(cluster);
            if (!nc) return 0;
            if (fat_flush()) return 0;

            uint32_t new_lba = clus_to_lba(nc);

            /* Zero the new cluster. */
            memset(s_sec, 0, 512);
            for (uint8_t s = 0; s < vol.spc; s++) {
                if (ide_write_sectors(vol.drive, new_lba + s, 1, s_sec))
                    return 0;
            }

            /* Place the entry in the first slot of the new cluster. */
            memcpy(s_sec, short_name, 11);
            s_sec[11] = attr;
            wr16(s_sec, 20, (uint16_t)(first_cluster >> 16));
            wr16(s_sec, 26, (uint16_t)(first_cluster));
            wr32(s_sec, 28, file_size);

            if (ide_write_sectors(vol.drive, new_lba, 1, s_sec))
                return 0;
            if (out_off) *out_off = 0;
            return new_lba;
        }
        cluster = next;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Public API - mount / unmount
 * ---------------------------------------------------------------------- */

int fat32_mount(uint8_t drive, uint32_t part_lba)
{
    if (ide_read_sectors(drive, part_lba, 1, s_sec))
        return -1;

    /* Basic FAT boot-sector signature. */
    if (s_sec[510] != 0x55u || s_sec[511] != 0xAAu)
        return -2;

    uint16_t bps    = rd16(s_sec, 11);
    uint8_t  spc    = s_sec[13];
    uint32_t rsvd   = rd16(s_sec, 14);
    uint8_t  nfats  = s_sec[16];
    uint16_t rc16   = rd16(s_sec, 17); /* root entry count - must be 0 */
    uint32_t ts16   = rd16(s_sec, 19); /* total sectors 16-bit */
    uint16_t spf16  = rd16(s_sec, 22); /* sectors/FAT 16-bit - must be 0 */
    uint32_t ts32   = rd32(s_sec, 32);
    uint32_t spf32  = rd32(s_sec, 36);
    uint32_t root_c = rd32(s_sec, 44);

    /* FAT32 validation: bytes/sector must be 512, root entry count 0,
       sectors/FAT-16 field must be 0. */
    if (bps != 512u || spc == 0u || rsvd == 0u || nfats == 0u
        || rc16 != 0u || spf16 != 0u || spf32 == 0u)
        return -2;

    uint32_t total_sectors = ts16 ? ts16 : ts32;
    if (total_sectors == 0u)
        return -2;

    vol.mounted       = 1;
    vol.drive         = drive;
    vol.part_lba      = part_lba;
    vol.spc           = spc;
    vol.rsvd          = rsvd;
    vol.nfats         = nfats;
    vol.spf           = spf32;
    vol.root_cluster  = root_c;
    vol.fat_lba       = part_lba + rsvd;
    vol.data_lba      = part_lba + rsvd + (uint32_t)nfats * spf32;
    vol.total_clusters =
        (total_sectors - rsvd - (uint32_t)nfats * spf32) / spc;
    vol.free_hint     = 2u;
    vol.cwd_cluster   = root_c;
    vol.cwd_path[0]   = '\\';
    vol.cwd_path[1]   = '\0';

    s_fat_lba   = 0xFFFFFFFFu;
    s_fat_dirty = 0;

    return 0;
}

void fat32_unmount(void)
{
    if (vol.mounted) {
        fat_flush();
        vol.mounted = 0;
    }
}

int fat32_mounted(void) { return vol.mounted; }

/* -------------------------------------------------------------------------
 * fat32_ls
 * ---------------------------------------------------------------------- */
int fat32_ls(const char *path)
{
    if (!vol.mounted) return -3;

    uint32_t cluster = path_resolve_dir(path);
    if (!cluster) {
        t_writestring("ls: not found or not a directory\n");
        return -1;
    }

    return dir_scan(cluster, DIRSCAN_LIST, NULL, NULL, NULL, NULL);
}

/* -------------------------------------------------------------------------
 * fat32_cd
 * ---------------------------------------------------------------------- */
int fat32_cd(const char *path)
{
    if (!vol.mounted) return -3;
    if (!path || !*path) return 0;

    /* Build the new absolute path string. */
    char new_path[256];
    int  np = 0;

    const char *p;
    if (is_sep(path[0])) {
        new_path[np++] = '\\';
        p = path + 1;
    } else {
        /* Start from cwd. */
        int len = (int)strlen(vol.cwd_path);
        if (len > 254) len = 254;
        memcpy(new_path, vol.cwd_path, (size_t)len);
        np = len;
        if (np > 1 && new_path[np - 1] != '\\')
            new_path[np++] = '\\';
        p = path;
    }

    /* Walk components, resolving . and .. in the string. */
    while (*p) {
        char comp[256];
        int  len = 0;
        while (*p && !is_sep(*p)) {
            if (len < 255) comp[len++] = *p;
            p++;
        }
        comp[len] = '\0';
        if (*p) p++;
        if (len == 0) continue;

        if (strcmp(comp, ".") == 0) continue;

        if (strcmp(comp, "..") == 0) {
            /* Strip the last component from new_path. */
            if (np > 1) {
                if (new_path[np - 1] == '\\') np--; /* trailing sep */
                while (np > 1 && new_path[np - 1] != '\\') np--;
            }
            continue;
        }

        /* Append component. */
        if (np > 1) new_path[np++] = '\\';
        for (int i = 0; comp[i] && np < 254; i++)
            new_path[np++] = comp[i];
    }
    new_path[np] = '\0';
    if (np == 0) { new_path[0] = '\\'; new_path[1] = '\0'; np = 1; }

    /* Resolve the built path to a cluster. */
    uint32_t new_cluster = path_resolve_dir(new_path);
    if (!new_cluster) return -1;

    vol.cwd_cluster = new_cluster;
    memcpy(vol.cwd_path, new_path, (size_t)(np + 1));
    return 0;
}

const char *fat32_getcwd(void) { return vol.cwd_path; }

/* -------------------------------------------------------------------------
 * fat32_mkdir
 * ---------------------------------------------------------------------- */
int fat32_mkdir(const char *path)
{
    if (!vol.mounted) return -3;

    uint32_t    parent_cluster;
    const char *basename;
    if (path_split(path, &parent_cluster, &basename)) return -1;
    if (!*basename) return -1;

    /* Fail if an entry with this name already exists. */
    dirent_t existing;
    if (dir_scan(parent_cluster, DIRSCAN_FIND, basename, &existing, NULL, NULL) == 0)
        return -6;

    /* Allocate one cluster for the new directory. */
    uint32_t nc = fat_alloc(0u);
    if (!nc) return -4;
    if (fat_flush()) return -2;

    uint32_t new_lba = clus_to_lba(nc);

    /* Zero the entire cluster (marks all entries as end-of-directory). */
    memset(s_sec, 0, 512);
    for (uint8_t s = 0; s < vol.spc; s++) {
        if (ide_write_sectors(vol.drive, new_lba + s, 1, s_sec))
            return -2;
    }

    /* Write . and .. into the first sector. */
    if (ide_read_sectors(vol.drive, new_lba, 1, s_sec))
        return -2;

    /* "." entry (byte 0..31) */
    memset(s_sec, ' ', 11);
    s_sec[0]  = '.';
    s_sec[11] = ATTR_DIR;
    wr16(s_sec, 20, (uint16_t)(nc >> 16));
    wr16(s_sec, 26, (uint16_t)(nc));
    wr32(s_sec, 28, 0);

    /* ".." entry (byte 32..63) */
    memset(s_sec + 32, ' ', 11);
    s_sec[32] = '.'; s_sec[33] = '.';
    s_sec[43] = ATTR_DIR;  /* offset 32+11 = 43 */
    /* Root directory is represented as cluster 0 in the .. entry. */
    uint32_t pp = (parent_cluster == vol.root_cluster) ? 0u : parent_cluster;
    wr16(s_sec + 32, 20, (uint16_t)(pp >> 16));
    wr16(s_sec + 32, 26, (uint16_t)(pp));
    wr32(s_sec + 32, 28, 0);

    if (ide_write_sectors(vol.drive, new_lba, 1, s_sec))
        return -2;

    /* Create the directory entry in the parent. */
    uint32_t ent_off;
    if (!dir_add_with_lfn(parent_cluster, basename, ATTR_DIR, nc, 0u, &ent_off))
        return -5;

    return 0;
}

/* -------------------------------------------------------------------------
 * fat32_read_file
 * ---------------------------------------------------------------------- */
int fat32_file_exists(const char *path)
{
    if (!vol.mounted) return 0;
    uint32_t    parent_cluster;
    const char *basename;
    if (path_split(path, &parent_cluster, &basename)) return 0;
    if (!*basename) return 0;
    dirent_t ent;
    if (dir_scan(parent_cluster, DIRSCAN_FIND, basename, &ent, NULL, NULL) != 0)
        return 0;
    return 1;
}

int fat32_read_file(const char *path, void *buf, uint32_t bufsz,
                    uint32_t *out_sz)
{
    if (!vol.mounted) return -3;

    uint32_t    parent_cluster;
    const char *basename;
    if (path_split(path, &parent_cluster, &basename)) return -1;
    if (!*basename) return -1;

    dirent_t ent;
    if (dir_scan(parent_cluster, DIRSCAN_FIND, basename, &ent, NULL, NULL) != 0)
        return -1;
    if (ent.attr & ATTR_DIR) return -1;

    uint32_t to_read = (ent.file_size < bufsz) ? ent.file_size : bufsz;
    if (out_sz) *out_sz = to_read;
    if (!buf || to_read == 0) return 0;

    uint8_t  *dst       = (uint8_t *)buf;
    uint32_t  remaining = to_read;
    uint32_t  cluster   = ent.first_cluster;

    while (remaining > 0u && cluster >= 2u && cluster < FAT32_BAD) {
        uint32_t lba = clus_to_lba(cluster);

        for (uint8_t s = 0; s < vol.spc && remaining > 0u; s++) {
            if (ide_read_sectors(vol.drive, lba + s, 1, s_sec))
                return -2;
            uint32_t chunk = remaining > 512u ? 512u : remaining;
            memcpy(dst, s_sec, chunk);
            dst       += chunk;
            remaining -= chunk;
        }
        cluster = fat_read(cluster);
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * fat32_write_file
 * ---------------------------------------------------------------------- */
int fat32_write_file(const char *path, const void *buf, uint32_t size)
{
    if (!vol.mounted) return -3;

    uint32_t    parent_cluster;
    const char *basename;
    if (path_split(path, &parent_cluster, &basename)) return -1;
    if (!*basename) return -1;

    /* Check for an existing entry. */
    dirent_t existing;
    int exists = (dir_scan(parent_cluster, DIRSCAN_FIND,
                           basename, &existing, NULL, NULL) == 0);
    if (exists && (existing.attr & ATTR_DIR)) return -1;

    /* Free the old cluster chain if overwriting. */
    if (exists && existing.first_cluster >= 2u)
        fat_free_chain(existing.first_cluster);

    /* Allocate a new cluster chain for the data. */
    uint32_t first_cluster = 0u;
    uint32_t prev          = 0u;

    if (size > 0u) {
        uint32_t bpc      = (uint32_t)vol.spc * 512u;
        uint32_t clusters = (size + bpc - 1u) / bpc;

        for (uint32_t i = 0u; i < clusters; i++) {
            uint32_t c = fat_alloc(prev);
            if (!c) { fat_flush(); return -4; }
            if (i == 0u) first_cluster = c;
            prev = c;
        }
    }

    if (fat_flush()) return -2;

    /* Write file data sector by sector. */
    const uint8_t *src       = (const uint8_t *)buf;
    uint32_t       remaining = size;
    uint32_t       cluster   = first_cluster;

    while (remaining > 0u && cluster >= 2u && cluster < FAT32_BAD) {
        uint32_t lba = clus_to_lba(cluster);

        for (uint8_t s = 0; s < vol.spc && remaining > 0u; s++) {
            uint32_t chunk = remaining > 512u ? 512u : remaining;
            memset(s_sec, 0, 512);
            memcpy(s_sec, src, chunk);
            if (ide_write_sectors(vol.drive, lba + s, 1, s_sec))
                return -2;
            src       += chunk;
            remaining -= chunk;
        }
        cluster = fat_read(cluster);
    }

    /* Update or create the directory entry. */
    if (exists) {
        if (ide_read_sectors(vol.drive, existing.ent_lba, 1, s_sec))
            return -2;
        uint8_t *e = s_sec + existing.ent_off;
        wr16(e, 20, (uint16_t)(first_cluster >> 16));
        wr16(e, 26, (uint16_t)(first_cluster));
        wr32(e, 28, size);
        if (ide_write_sectors(vol.drive, existing.ent_lba, 1, s_sec))
            return -2;
    } else {
        uint32_t ent_off;
        if (!dir_add_with_lfn(parent_cluster, basename, ATTR_ARCH,
                              first_cluster, size, &ent_off))
            return -5;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * fat32_delete_file - delete a file, freeing its cluster chain.
 * Returns 0 on success, negative on error.
 * ---------------------------------------------------------------------- */
int fat32_delete_file(const char *path)
{
    if (!vol.mounted) return -3;

    uint32_t    parent_cluster;
    const char *basename;
    if (path_split(path, &parent_cluster, &basename)) return -1;
    if (!*basename) return -1;

    dirent_t ent;
    if (dir_scan(parent_cluster, DIRSCAN_FIND, basename, &ent, NULL, NULL)) return -1;
    if (ent.attr & ATTR_DIR) return -1; /* is a directory, not a file */

    /* Mark the 8.3 directory entry as deleted (0xE5). */
    if (ide_read_sectors(vol.drive, ent.ent_lba, 1, s_sec)) return -2;
    s_sec[ent.ent_off] = 0xE5u;
    if (ide_write_sectors(vol.drive, ent.ent_lba, 1, s_sec)) return -2;

    /* Free the data cluster chain. */
    if (ent.first_cluster >= 2u)
        fat_free_chain(ent.first_cluster);
    return fat_flush();
}

/* -------------------------------------------------------------------------
 * fat32_delete_dir - delete an empty directory.
 * Returns 0 on success, -5 if not empty, negative on other errors.
 * ---------------------------------------------------------------------- */
int fat32_delete_dir(const char *path)
{
    if (!vol.mounted) return -3;

    uint32_t    parent_cluster;
    const char *basename;
    if (path_split(path, &parent_cluster, &basename)) return -1;
    if (!*basename) return -1;

    dirent_t ent;
    if (dir_scan(parent_cluster, DIRSCAN_FIND, basename, &ent, NULL, NULL)) return -1;
    if (!(ent.attr & ATTR_DIR)) return -1; /* not a directory */

    /* Verify the directory is empty (only . and .. entries allowed). */
    uint32_t dir_cluster = ent.first_cluster ? ent.first_cluster : vol.root_cluster;
    int empty = 0;

    for (uint32_t c = dir_cluster; !empty && c >= 2u && c < FAT32_BAD; c = fat_read(c)) {
        uint32_t base_lba = clus_to_lba(c);
        for (uint8_t s = 0; !empty && s < vol.spc; s++) {
            if (ide_read_sectors(vol.drive, base_lba + s, 1, s_sec)) return -2;
            for (int i = 0; i < 16; i++) {
                uint8_t *e = s_sec + i * 32;
                if (e[0] == 0x00u) { empty = 1; break; }
                if ((uint8_t)e[0] == 0xE5u) continue;
                uint8_t a = e[11];
                if ((a & 0x3Fu) == ATTR_LFN || (a & ATTR_VOLID)) continue;
                /* . and .. entries */
                if (e[0] == '.' && e[1] == ' ') continue;
                if (e[0] == '.' && e[1] == '.') continue;
                return -5; /* not empty */
            }
        }
    }

    /* Mark the directory entry as deleted. */
    if (ide_read_sectors(vol.drive, ent.ent_lba, 1, s_sec)) return -2;
    s_sec[ent.ent_off] = 0xE5u;
    if (ide_write_sectors(vol.drive, ent.ent_lba, 1, s_sec)) return -2;

    /* Free the directory cluster chain. */
    if (dir_cluster >= 2u)
        fat_free_chain(dir_cluster);
    return fat_flush();
}

/* -------------------------------------------------------------------------
 * fat32_rename_file / fat32_rename_dir
 *
 * Both use the same logic: create a new directory entry at new_path pointing
 * to the same cluster chain, then mark the old entry as deleted.
 * The cluster chain data is not touched.
 *
 * Returns 0 on success, negative on error.
 * ---------------------------------------------------------------------- */
static int fat32_rename_internal(const char *old_path, const char *new_path)
{
    if (!vol.mounted) return -3;

    uint32_t    old_parent;
    const char *old_base;
    if (path_split(old_path, &old_parent, &old_base)) return -1;
    if (!*old_base) return -1;

    uint32_t    new_parent;
    const char *new_base;
    if (path_split(new_path, &new_parent, &new_base)) return -1;
    if (!*new_base) return -1;

    /* Find the source entry. */
    dirent_t ent;
    if (dir_scan(old_parent, DIRSCAN_FIND, old_base, &ent, NULL, NULL)) return -1;

    /* Destination must not already exist. */
    dirent_t existing;
    if (dir_scan(new_parent, DIRSCAN_FIND, new_base, &existing, NULL, NULL) == 0) return -6;

    /* Create the new directory entry with the same cluster/size/attr. */
    uint32_t dummy;
    if (!dir_add_with_lfn(new_parent, new_base, ent.attr,
                          ent.first_cluster, ent.file_size, &dummy))
        return -5;

    /* Re-find the old entry (dir_add_with_lfn may have flushed FAT/sector cache). */
    if (dir_scan(old_parent, DIRSCAN_FIND, old_base, &ent, NULL, NULL)) return -7;

    /* Mark the old 8.3 entry as deleted. */
    if (ide_read_sectors(vol.drive, ent.ent_lba, 1, s_sec)) return -2;
    s_sec[ent.ent_off] = 0xE5u;
    if (ide_write_sectors(vol.drive, ent.ent_lba, 1, s_sec)) return -2;

    return fat_flush();
}

int fat32_rename_file(const char *old_path, const char *new_path)
{
    return fat32_rename_internal(old_path, new_path);
}

int fat32_rename_dir(const char *old_path, const char *new_path)
{
    return fat32_rename_internal(old_path, new_path);
}

/* -------------------------------------------------------------------------
 * fat32_mkfs - format a partition as FAT32
 * ---------------------------------------------------------------------- */
int fat32_mkfs(uint8_t drive, uint32_t part_lba, uint32_t part_sectors)
{
    if (part_sectors < 66000u) return -6; /* too small for FAT32 */

    /* Choose sectors-per-cluster based on partition size. */
    uint8_t spc;
    if      (part_sectors <= 131072u)   spc = 1;
    else if (part_sectors <= 524288u)   spc = 4;
    else if (part_sectors <= 4194304u)  spc = 8;
    else                                spc = 16;

    uint32_t reserved = 32u;
    uint8_t  nfats    = 2u;

    /* Compute sectors-per-FAT.
     * Approximate total_clusters = (part_sectors - reserved) / spc, then
     * spf = ceil(total_clusters * 4 / 512) + 1 for safety. */
    uint32_t tc_approx = (part_sectors - reserved) / spc;
    uint32_t spf       = (tc_approx * 4u + 511u) / 512u + 1u;

    uint32_t data_sectors    = part_sectors - reserved - nfats * spf;
    uint32_t total_clusters  = data_sectors / spc;

    if (total_clusters < 65525u) return -6;

    /* ---- BPB sector ---- */
    memset(s_sec, 0, 512);
    s_sec[0] = 0xEBu; s_sec[1] = 0x58u; s_sec[2] = 0x90u; /* jmp + nop */
    memcpy(s_sec + 3, "MSDOS5.0", 8);
    wr16(s_sec, 11, 512u);
    s_sec[13] = spc;
    wr16(s_sec, 14, (uint16_t)reserved);
    s_sec[16] = nfats;
    wr16(s_sec, 17, 0);           /* root entry count   = 0 */
    wr16(s_sec, 19, 0);           /* total sectors 16   = 0 */
    s_sec[21] = 0xF8u;            /* media type         */
    wr16(s_sec, 22, 0);           /* sectors/FAT 16     = 0 */
    wr16(s_sec, 24, 63u);         /* sectors per track  */
    wr16(s_sec, 26, 255u);        /* number of heads    */
    wr32(s_sec, 28, part_lba);    /* hidden sectors     */
    wr32(s_sec, 32, part_sectors);
    wr32(s_sec, 36, spf);
    wr16(s_sec, 40, 0);           /* flags              */
    wr16(s_sec, 42, 0);           /* FS version         */
    wr32(s_sec, 44, 2u);          /* root cluster       */
    wr16(s_sec, 48, 1u);          /* FSInfo sector      */
    wr16(s_sec, 50, 6u);          /* backup boot sector */
    s_sec[64] = 0x80u;            /* drive number       */
    s_sec[66] = 0x29u;            /* boot signature     */
    wr32(s_sec, 67, part_lba ^ 0xA5A5A5A5u); /* volume ID */
    memcpy(s_sec + 71, "MAKAR      ", 11);
    memcpy(s_sec + 82, "FAT32   ",   8);
    s_sec[510] = 0x55u; s_sec[511] = 0xAAu;

    if (ide_write_sectors(drive, part_lba,     1u, s_sec)) return -2;
    if (ide_write_sectors(drive, part_lba + 6u, 1u, s_sec)) return -2;

    /* ---- FSInfo sector (sector 1) ---- */
    memset(s_sec, 0, 512);
    wr32(s_sec,   0, 0x41615252u);              /* lead signature      */
    wr32(s_sec, 484, 0x61417272u);              /* structure signature */
    wr32(s_sec, 488, total_clusters - 1u);      /* free cluster count  */
    wr32(s_sec, 492, 3u);                       /* next free cluster   */
    wr32(s_sec, 508, 0xAA550000u);              /* trail signature     */
    if (ide_write_sectors(drive, part_lba + 1u, 1u, s_sec)) return -2;

    /* ---- FAT1 and FAT2 ---- */
    uint32_t fat1_lba = part_lba + reserved;
    uint32_t fat2_lba = fat1_lba + spf;

    /* First FAT sector: media-type word, EOC for cluster 1, EOC for root. */
    memset(s_sec, 0, 512);
    wr32(s_sec,  0, 0x0FFFFFF8u);
    wr32(s_sec,  4, 0x0FFFFFFFu);
    wr32(s_sec,  8, 0x0FFFFFFFu);
    if (ide_write_sectors(drive, fat1_lba, 1u, s_sec)) return -2;
    if (ide_write_sectors(drive, fat2_lba, 1u, s_sec)) return -2;

    /* Zero remaining FAT sectors. */
    memset(s_sec, 0, 512);
    for (uint32_t i = 1u; i < spf; i++) {
        if (ide_write_sectors(drive, fat1_lba + i, 1u, s_sec)) return -2;
        if (ide_write_sectors(drive, fat2_lba + i, 1u, s_sec)) return -2;
    }

    /* ---- Root directory cluster (cluster 2) ---- */
    uint32_t root_lba = part_lba + reserved + nfats * spf; /* == data_lba + 0 */
    for (uint8_t s = 0; s < spc; s++) {
        if (ide_write_sectors(drive, root_lba + s, 1u, s_sec)) return -2;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * fat32_complete - enumerate entries in dir_path, calling cb for each.
 * The callback receives the entry name, a is_dir flag, and the ctx pointer.
 * Prefix filtering is left to the caller.
 * ---------------------------------------------------------------------- */
int fat32_complete(const char *dir_path, const char *prefix,
                   fat32_complete_cb_t cb, void *ctx)
{
    (void)prefix;   /* caller does prefix filtering in the callback */
    if (!vol.mounted) return -1;
    uint32_t cluster = path_resolve_dir(dir_path);
    if (!cluster) return -1;
    dir_scan(cluster, DIRSCAN_COMPLETE, NULL, NULL, cb, ctx);
    return 0;
}
