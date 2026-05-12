/*
 * iso9660.c - read-only ISO9660 filesystem driver.
 *
 * Supports:
 *   - Primary Volume Descriptor parsing (sector 16)
 *   - Directory record traversal and path resolution
 *   - File content reading via ide_read_atapi_sectors()
 *   - Rock Ridge RRIP "NM" entries for long filenames (IEEE P1282)
 *
 * File identifiers are matched case-insensitively.  When a Rock Ridge
 * alternate name ("NM" System Use entry) is present it is preferred over
 * the truncated ISO9660 Level-1 8.3 identifier in the PVD, allowing names
 * like "makar.kernel" and "part_msdos.mod" to be resolved correctly.  The
 * ";1" ISO9660 version suffix is stripped before comparison of plain names.
 *
 * All sector buffers are declared at file scope to avoid large stack frames.
 */

#include <kernel/iso9660.h>
#include <kernel/ide.h>
#include <kernel/tty.h>
#include <string.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * ISO9660 constants
 * ---------------------------------------------------------------------- */

/* LBA of the Primary Volume Descriptor on a standard ISO9660 disc. */
#define PVD_SECTOR            16u

/* Volume descriptor type codes. */
#define VD_TYPE_PRIMARY        1u

/* Standard identifier that must appear at bytes 1–5 of every descriptor. */
#define VD_ID                 "CD001"

/* ISO9660 directory record: flags byte bit definitions. */
#define ISO_FLAG_DIR          0x02u   /* entry is a sub-directory */

/* Rock Ridge RRIP NM entry flag bits (IEEE P1282 §4.1.4). */
#define NM_FLAG_CONTINUE      0x01u   /* name continues in next NM entry     */
#define NM_FLAG_CURRENT       0x02u   /* name refers to "." (current dir)    */
#define NM_FLAG_PARENT        0x04u   /* name refers to ".." (parent dir)    */

/*
 * Maximum length of a single path component we handle.
 * ISO9660 level 1 allows 8+3 characters; level 2 allows up to 207.
 * We use 220 to comfortably cover level 2 plus the ";N" suffix.
 */
#define ISO_MAX_COMPONENT_LEN  220

/* -------------------------------------------------------------------------
 * Static sector buffer - never on the kernel stack (2048 bytes).
 * ---------------------------------------------------------------------- */
static uint8_t s_sector[ISO9660_SECTOR_SIZE];

/* -------------------------------------------------------------------------
 * Little-endian 32-bit read helper.
 * ISO9660 stores numeric fields in both byte orders; we always read LE.
 * ---------------------------------------------------------------------- */
static uint32_t iso_rd32le(const uint8_t *b)
{
    return  (uint32_t)b[0]
          | ((uint32_t)b[1] <<  8)
          | ((uint32_t)b[2] << 16)
          | ((uint32_t)b[3] << 24);
}

/* -------------------------------------------------------------------------
 * rr_get_name – extract the Rock Ridge Alternate Name ("NM") from the
 * System Use Area of a directory record.
 *
 * Rock Ridge (IEEE P1282) stores the real POSIX filename in one or more
 * "NM" System Use entries that follow the padded ISO9660 file identifier.
 * grub-mkrescue always generates Rock Ridge data, so we prefer these names
 * over the truncated 8.3 Level-1 identifiers in the PVD.
 *
 * rec   : pointer to the start of the directory record in the sector buffer
 * buf   : output buffer for the assembled name (NUL-terminated)
 * bufsz : capacity of buf (including the NUL)
 *
 * Returns the length of the name placed in buf, or 0 if no usable NM entry
 * was found.
 * ---------------------------------------------------------------------- */
static int rr_get_name(const uint8_t *rec, char *buf, int bufsz)
{
    uint8_t rec_len  = rec[0];
    uint8_t name_len = rec[32];

    /*
     * System Use Area starts immediately after the file identifier, padded
     * to an even byte boundary within the record.
     * Identifier starts at byte 33; its length is name_len.
     * If (33 + name_len) is odd, one pad byte is inserted to reach an even
     * offset before the System Use entries begin.
     */
    int su_off = 33 + (int)name_len;
    if (su_off & 1)      /* odd → insert 1 pad byte to reach even offset */
        su_off++;

    int out_len = 0;
    if (bufsz > 0)
        buf[0] = '\0';

    while (su_off + 4 <= (int)rec_len) {
        uint8_t su_len = rec[su_off + 2];
        if (su_len < 4)
            break;

        if (rec[su_off] == 'N' && rec[su_off + 1] == 'M') {
            /*
             * NM entry layout (RRIP 1.12 §4.1.4):
             *   bytes 0-1 : signature "NM"
             *   byte  2   : length (including these 5 header bytes)
             *   byte  3   : version (must be 1)
             *   byte  4   : flags
             *   bytes 5.. : name fragment
             *
             * Flag bits:
             *   0x01 = CONTINUE  – more NM entries follow
             *   0x02 = CURRENT   – name is "."
             *   0x04 = PARENT    – name is ".."
             */
            uint8_t flags  = rec[su_off + 4];
            int     nm_len = (int)su_len - 5;

            /* Skip "." and ".." synthetic entries. */
            if ((flags & (NM_FLAG_CURRENT | NM_FLAG_PARENT)) == 0u && nm_len > 0) {
                const char *nm = (const char *)(rec + su_off + 5);
                int copy = nm_len;
                if (copy > bufsz - out_len - 1)
                    copy = bufsz - out_len - 1;
                if (copy > 0) {
                    memcpy(buf + out_len, nm, (size_t)copy);
                    out_len += copy;
                    buf[out_len] = '\0';
                }
                /* If the CONTINUE flag is clear the name is complete. */
                if (!(flags & NM_FLAG_CONTINUE))
                    return out_len;
            }
        }

        su_off += (int)su_len;
    }

    return out_len;  /* 0 → no NM entry found */
}

/* -------------------------------------------------------------------------
 * iso_namecmp – case-insensitive compare of an ISO9660 identifier against
 * a NUL-terminated name string.
 *
 * 'ident' is NOT NUL-terminated; its length is 'ilen'.
 * The ";N" version suffix and a trailing "." are stripped from 'ident'
 * before comparison.
 *
 * Returns 0 if the names match, non-zero otherwise.
 * ---------------------------------------------------------------------- */
static int iso_namecmp(const char *ident, int ilen, const char *name)
{
    /* Strip ";N" version suffix. */
    if (ilen >= 2 && ident[ilen - 2] == ';')
        ilen -= 2;

    /* Strip a trailing dot (directory entries for files with no extension). */
    if (ilen > 0 && ident[ilen - 1] == '.')
        ilen--;

    int nlen = (int)strlen(name);
    if (ilen != nlen)
        return 1;

    for (int i = 0; i < ilen; i++) {
        unsigned char ca = (unsigned char)ident[i];
        unsigned char cb = (unsigned char)name[i];
        if (ca >= 'a' && ca <= 'z') ca = (unsigned char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (unsigned char)(cb - 32);
        if (ca != cb)
            return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * pvd_read – read the Primary Volume Descriptor and extract the root
 * directory's extent LBA and data length.
 *
 * Returns 0 on success, -1 on I/O error, -2 if not a valid ISO9660 PVD.
 * ---------------------------------------------------------------------- */
static int pvd_read(uint8_t drive, uint32_t *root_lba, uint32_t *root_size)
{
    if (ide_read_atapi_sectors(drive, PVD_SECTOR, 1, s_sector))
        return -1;

    /* Validate type byte and standard identifier. */
    if (s_sector[0] != VD_TYPE_PRIMARY)
        return -2;
    if (memcmp(s_sector + 1, VD_ID, 5) != 0)
        return -2;

    /*
     * Root directory record begins at byte 156 of the PVD (34 bytes).
     *   bytes 156+2 .. 156+5 : extent location (LE32)
     *   bytes 156+10 .. 156+13 : data length (LE32)
     */
    *root_lba  = iso_rd32le(s_sector + 156 + 2);
    *root_size = iso_rd32le(s_sector + 156 + 10);

    if (*root_lba == 0 || *root_size == 0)
        return -2;

    return 0;
}

/* -------------------------------------------------------------------------
 * dir_find – search a directory for a named entry.
 *
 * dir_lba  : LBA of the first sector of the directory data
 * dir_size : total size of the directory data in bytes
 * name     : NUL-terminated component name to find (case-insensitive)
 * out_lba  : set to the extent LBA of the matching entry
 * out_size : set to the data length of the matching entry
 * out_isdir: set to 1 if the entry is a directory, 0 if a file
 *
 * Returns  0 if found.
 * Returns -1 if not found.
 * Returns -2 on I/O error.
 * ---------------------------------------------------------------------- */
static int dir_find(uint8_t drive,
                    uint32_t dir_lba, uint32_t dir_size,
                    const char *name,
                    uint32_t *out_lba, uint32_t *out_size, int *out_isdir)
{
    uint32_t sectors = (dir_size + ISO9660_SECTOR_SIZE - 1u)
                       / ISO9660_SECTOR_SIZE;

    for (uint32_t s = 0; s < sectors; s++) {
        if (ide_read_atapi_sectors(drive, dir_lba + s, 1, s_sector))
            return -2;

        uint32_t pos = 0;
        while (pos < ISO9660_SECTOR_SIZE) {
            uint8_t rec_len = s_sector[pos];

            /* Zero record length marks the end of entries in this sector. */
            if (rec_len == 0)
                break;

            uint8_t name_len = s_sector[pos + 32];
            const char *ident = (const char *)(s_sector + pos + 33);

            /* Skip "." (name_len=1, ident[0]=0x00) and
               ".." (name_len=1, ident[0]=0x01). */
            if (name_len == 1 &&
                ((uint8_t)ident[0] == 0x00u || (uint8_t)ident[0] == 0x01u))
            {
                pos += rec_len;
                continue;
            }

            /*
             * Prefer the Rock Ridge alternate name (stored in NM System Use
             * entries) over the ISO9660 Level-1 8.3 identifier.  This lets
             * us match names like "makar.kernel" or "part_msdos.mod" that
             * would otherwise be truncated in the PVD.
             */
            char rr_name[ISO_MAX_COMPONENT_LEN + 1];
            int  rr_len = rr_get_name(s_sector + pos, rr_name, (int)sizeof(rr_name));

            int matched;
            if (rr_len > 0)
                matched = (iso_namecmp(rr_name, rr_len, name) == 0);
            else
                matched = (iso_namecmp(ident, (int)name_len, name) == 0);

            if (matched) {
                *out_lba   = iso_rd32le(s_sector + pos + 2);
                *out_size  = iso_rd32le(s_sector + pos + 10);
                *out_isdir = (s_sector[pos + 25] & ISO_FLAG_DIR) ? 1 : 0;
                return 0;
            }

            pos += rec_len;
        }
    }

    return -1; /* not found */
}

/* -------------------------------------------------------------------------
 * path_resolve – resolve an absolute '/' path to an extent LBA and size.
 *
 * path     : must start with '/' (e.g. "/boot/grub/grub.cfg")
 * out_lba  : set to the extent LBA of the resolved entry
 * out_size : set to the data length of the resolved entry
 * out_isdir: set to 1 if the resolved entry is a directory
 *
 * Returns  0 on success.
 * Returns -1 on I/O error.
 * Returns -2 if any path component is not found.
 * ---------------------------------------------------------------------- */
static int path_resolve(uint8_t drive, const char *path,
                        uint32_t *out_lba, uint32_t *out_size, int *out_isdir)
{
    uint32_t cur_lba, cur_size;
    int      cur_isdir = 1;

    /* Load the root directory from the PVD. */
    if (pvd_read(drive, &cur_lba, &cur_size))
        return -1;

    /* Skip the leading '/'. */
    const char *p = path;
    if (*p == '/')
        p++;

    /* An empty path after the slash refers to the root directory itself. */
    if (*p == '\0') {
        *out_lba   = cur_lba;
        *out_size  = cur_size;
        *out_isdir = 1;
        return 0;
    }

    while (*p) {
        /* Can only descend into directories. */
        if (!cur_isdir)
            return -2;

        /* Extract the next path component (up to the next '/' or end). */
        char comp[ISO_MAX_COMPONENT_LEN + 2];  /* +2: NUL + safety margin */
        int  clen = 0;
        while (*p && *p != '/') {
            if (clen < ISO_MAX_COMPONENT_LEN)
                comp[clen++] = *p;
            p++;
        }
        comp[clen] = '\0';
        if (*p == '/')
            p++;
        if (clen == 0)
            continue;

        uint32_t next_lba = 0, next_size = 0;
        int      next_isdir = 0;

        int r = dir_find(drive, cur_lba, cur_size,
                         comp, &next_lba, &next_size, &next_isdir);
        if (r != 0)
            return r;  /* -1 = not found, -2 = I/O error */

        cur_lba   = next_lba;
        cur_size  = next_size;
        cur_isdir = next_isdir;
    }

    *out_lba   = cur_lba;
    *out_size  = cur_size;
    *out_isdir = cur_isdir;
    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int iso9660_probe(uint8_t drive)
{
    const ide_drive_t *d = ide_get_drive(drive);
    if (!d || !d->present || d->type != IDE_TYPE_ATAPI)
        return -2;

    uint32_t root_lba, root_size;
    return pvd_read(drive, &root_lba, &root_size);
}

int iso9660_read_file(uint8_t drive, const char *path,
                      void *buf, uint32_t bufsz, uint32_t *out_sz)
{
    uint32_t lba, size;
    int isdir;

    int err = path_resolve(drive, path, &lba, &size, &isdir);
    if (err)
        return err;
    if (isdir)
        return -2;

    uint32_t to_read = (size < bufsz) ? size : bufsz;
    if (out_sz)
        *out_sz = to_read;
    if (!buf || to_read == 0)
        return 0;

    uint8_t *dst       = (uint8_t *)buf;
    uint32_t remaining = to_read;
    uint32_t sect_lba  = lba;

    while (remaining > 0u) {
        if (ide_read_atapi_sectors(drive, sect_lba, 1, s_sector))
            return -1;

        uint32_t chunk = (remaining > ISO9660_SECTOR_SIZE)
                         ? ISO9660_SECTOR_SIZE : remaining;
        memcpy(dst, s_sector, chunk);
        dst       += chunk;
        remaining -= chunk;
        sect_lba++;
    }

    return 0;
}

int iso9660_file_exists(uint8_t drive, const char *path)
{
    uint32_t lba, size;
    int isdir;
    if (path_resolve(drive, path, &lba, &size, &isdir) != 0)
        return 0;
    return 1;
}

int iso9660_ls(uint8_t drive, const char *path)
{
    uint32_t lba, size;
    int isdir;

    int err = path_resolve(drive, path, &lba, &size, &isdir);
    if (err) {
        t_writestring("iso9660: path not found\n");
        return err;
    }
    if (!isdir) {
        t_writestring("iso9660: not a directory\n");
        return -2;
    }

    uint32_t sectors = (size + ISO9660_SECTOR_SIZE - 1u) / ISO9660_SECTOR_SIZE;

    for (uint32_t s = 0; s < sectors; s++) {
        if (ide_read_atapi_sectors(drive, lba + s, 1, s_sector))
            return -1;

        uint32_t pos = 0;
        while (pos < ISO9660_SECTOR_SIZE) {
            uint8_t rec_len  = s_sector[pos];
            if (rec_len == 0)
                break;

            uint8_t     name_len = s_sector[pos + 32];
            const char *ident    = (const char *)(s_sector + pos + 33);
            uint8_t     flags    = s_sector[pos + 25];
            uint32_t    fsz      = iso_rd32le(s_sector + pos + 10);

            /* Skip "." and ".." entries. */
            if (name_len == 1 &&
                ((uint8_t)ident[0] == 0x00u || (uint8_t)ident[0] == 0x01u))
            {
                pos += rec_len;
                continue;
            }

            /*
             * Prefer the Rock Ridge alternate name for display; fall back to
             * the ISO9660 identifier (stripping ";N" suffix and trailing dot).
             */
            char rr_name[ISO_MAX_COMPONENT_LEN + 1];
            int  rr_len = rr_get_name(s_sector + pos, rr_name, (int)sizeof(rr_name));

            if (flags & ISO_FLAG_DIR)
                t_putchar('[');

            if (rr_len > 0) {
                for (int i = 0; i < rr_len; i++)
                    t_putchar(rr_name[i]);
            } else {
                /* Strip ";N" version suffix and trailing dot. */
                int nlen = (int)name_len;
                if (nlen >= 2 && ident[nlen - 2] == ';')
                    nlen -= 2;
                if (nlen > 0 && ident[nlen - 1] == '.')
                    nlen--;
                for (int i = 0; i < nlen; i++)
                    t_putchar(ident[i]);
            }

            if (flags & ISO_FLAG_DIR) {
                t_putchar(']');
            } else {
                t_writestring("  (");
                t_dec(fsz);
                t_writestring(" B)");
            }
            t_putchar('\n');

            pos += rec_len;
        }
    }

    return 0;
}

int iso9660_complete(uint8_t drive, const char *dir_path, const char *prefix,
                     iso9660_complete_cb_t cb, void *ctx)
{
    (void)prefix;   /* caller filters by prefix in cb */

    uint32_t lba, size;
    int isdir;
    int err = path_resolve(drive, dir_path, &lba, &size, &isdir);
    if (err)
        return err;
    if (!isdir)
        return -2;

    uint32_t sectors = (size + ISO9660_SECTOR_SIZE - 1u) / ISO9660_SECTOR_SIZE;

    for (uint32_t s = 0; s < sectors; s++) {
        if (ide_read_atapi_sectors(drive, lba + s, 1, s_sector))
            return -1;

        uint32_t pos = 0;
        while (pos < ISO9660_SECTOR_SIZE) {
            uint8_t rec_len = s_sector[pos];
            if (rec_len == 0)
                break;

            uint8_t     name_len = s_sector[pos + 32];
            const char *ident    = (const char *)(s_sector + pos + 33);
            uint8_t     flags    = s_sector[pos + 25];

            /* Skip "." and ".." entries. */
            if (name_len == 1 &&
                ((uint8_t)ident[0] == 0x00u || (uint8_t)ident[0] == 0x01u))
            {
                pos += rec_len;
                continue;
            }

            char name[ISO_MAX_COMPONENT_LEN + 1];
            int  nlen = rr_get_name(s_sector + pos, name, (int)sizeof(name));
            if (nlen <= 0) {
                /* Fallback: ISO9660 identifier with ;N and trailing dot
                 * stripped - same cleanup iso9660_ls applies. */
                int il = (int)name_len;
                if (il >= 2 && ident[il - 2] == ';') il -= 2;
                if (il > 0  && ident[il - 1] == '.') il--;
                if (il > ISO_MAX_COMPONENT_LEN) il = ISO_MAX_COMPONENT_LEN;
                for (int i = 0; i < il; i++) name[i] = ident[i];
                name[il] = '\0';
                nlen = il;
            }
            if (nlen > 0)
                cb(name, (flags & ISO_FLAG_DIR) ? 1 : 0, ctx);

            pos += rec_len;
        }
    }
    return 0;
}
