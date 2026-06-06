/*
 * devfs.c - synthetic /dev filesystem.  See kernel/devfs.h.
 */

#include <kernel/devfs.h>
#include <kernel/ide.h>
#include <kernel/partition.h>
#include <kernel/tty.h>
#include <kernel/vtty.h>
#include <string.h>

#define DEVFS_MAX_NODES   32
#define ATA_SECTOR        512u
#define ATAPI_SECTOR      2048u

typedef enum {
    DEV_DISK = 0,   /* whole ATA disk          */
    DEV_PART,       /* ATA partition window    */
    DEV_CDROM,      /* ATAPI optical drive     */
    DEV_TTY,        /* virtual terminal (/dev/ttyN -> vtty slot) */
} dev_kind_t;

typedef struct {
    char       name[16];   /* node name, no leading '/' (e.g. "hda1")   */
    uint8_t    drive;      /* IDE drive index 0-3 (DEV_TTY: vtty slot)  */
    uint8_t    kind;       /* dev_kind_t                                 */
    uint8_t    readonly;   /* 1 = writes rejected (CD-ROM)               */
    uint32_t   base_lba;   /* first sector of the window                */
    uint32_t   sectors;    /* window length in native sectors           */
} dev_node_t;

static dev_node_t s_nodes[DEVFS_MAX_NODES];
static int        s_count;

/* ------------------------------------------------------------------------- */

static uint32_t node_sector_size(const dev_node_t *n)
{
    return (n->kind == DEV_CDROM) ? ATAPI_SECTOR : ATA_SECTOR;
}

static void add_node(const char *name, uint8_t drive, dev_kind_t kind,
                     uint8_t readonly, uint32_t base_lba, uint32_t sectors)
{
    if (s_count >= DEVFS_MAX_NODES) return;
    dev_node_t *n = &s_nodes[s_count++];
    size_t i = 0;
    for (; name[i] && i < sizeof(n->name) - 1; i++) n->name[i] = name[i];
    n->name[i]   = '\0';
    n->drive     = drive;
    n->kind      = (uint8_t)kind;
    n->readonly  = readonly;
    n->base_lba  = base_lba;
    n->sectors   = sectors;
}

void devfs_init(void)
{
    s_count = 0;

    /* ATA disk letter: hda, hdb, ... assigned per detected ATA drive in
     * drive-index order.  ATAPI drives become /dev/cdrom instead. */
    char disk_letter = 'a';

    for (uint8_t d = 0; d < IDE_MAX_DRIVES; d++) {
        const ide_drive_t *drv = ide_get_drive(d);
        if (!drv || !drv->present) continue;

        if (drv->type == IDE_TYPE_ATAPI) {
            /* ATAPI IDENTIFY has no usable LBA range; query the medium. */
            uint32_t cd_sectors = 0, cd_secsz = 0;
            if (ide_atapi_capacity(d, &cd_sectors, &cd_secsz) != 0)
                cd_sectors = 0;
            add_node("cdrom", d, DEV_CDROM, 1, 0, cd_sectors);
            continue;
        }
        if (drv->type != IDE_TYPE_ATA) continue;

        char disk[16];
        disk[0] = 'h'; disk[1] = 'd'; disk[2] = disk_letter; disk[3] = '\0';
        add_node(disk, d, DEV_DISK, 0, 0, drv->size);

        /* Enumerate partitions on this disk. */
        disk_parts_t parts;
        if (part_probe(d, &parts) == 0 && parts.scheme != PART_SCHEME_NONE) {
            for (int p = 0; p < parts.count && p < 9; p++) {
                const part_info_t *pi = &parts.parts[p];
                if (pi->lba_count == 0) continue;
                char pname[16];
                int o = 0;
                pname[o++] = 'h'; pname[o++] = 'd'; pname[o++] = disk_letter;
                /* Partition numbers are 1-based. */
                int num = p + 1;
                pname[o++] = (char)('0' + num);
                pname[o]   = '\0';
                add_node(pname, d, DEV_PART, 0, pi->lba_start, pi->lba_count);
            }
        }

        disk_letter++;
    }

    /* Virtual terminals: /dev/tty0 is the root console (VTTY_ROOT_SLOT) and
     * /dev/tty1../dev/tty9 are the nine user VT slots (slot == ttyN-1).  These
     * are name/route nodes, not block-backed: reads are EOF, writes paint the
     * slot's backing grid via vtty_write.  Always present so userspace can
     * address a VT by a stable Linux-style path regardless of makmux state. */
    for (int t = 0; t <= VTTY_SHELL_MAX; t++) {   /* tty0..tty9 */
        char nm[16];
        int o = 0;
        nm[o++] = 't'; nm[o++] = 't'; nm[o++] = 'y';
        nm[o++] = (char)('0' + t);
        nm[o]   = '\0';
        /* drive field carries the vtty slot: tty0 -> root, ttyN -> slot N-1. */
        uint8_t slot = (t == 0) ? (uint8_t)VTTY_ROOT_SLOT : (uint8_t)(t - 1);
        add_node(nm, slot, DEV_TTY, 0, 0, 0);
    }
}

/* ------------------------------------------------------------------------- */

static const char *rel_name(const char *path)
{
    /* devfs-relative path always starts with '/'.  "/" is the root. */
    if (!path || path[0] != '/') return NULL;
    if (path[1] == '\0') return NULL;   /* root, not a node */
    return path + 1;
}

int devfs_lookup(const char *path)
{
    const char *name = rel_name(path);
    if (!name) return -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(name, s_nodes[i].name) == 0) return i;
    }
    return -1;
}

int devfs_file_exists(const char *path)
{
    return devfs_lookup(path) >= 0 ? 1 : 0;
}

uint32_t devfs_node_size(int idx)
{
    if (idx < 0 || idx >= s_count) return 0;
    return s_nodes[idx].sectors * node_sector_size(&s_nodes[idx]);
}

int devfs_node_readonly(int idx)
{
    if (idx < 0 || idx >= s_count) return 1;
    return s_nodes[idx].readonly ? 1 : 0;
}

int devfs_node_location(int idx, uint8_t *out_drive, uint32_t *out_base_lba)
{
    if (idx < 0 || idx >= s_count) return -1;
    if (s_nodes[idx].kind == DEV_TTY) return -1;   /* not a block device */
    if (out_drive)    *out_drive    = s_nodes[idx].drive;
    if (out_base_lba) *out_base_lba = s_nodes[idx].base_lba;
    return 0;
}

/* ------------------------------------------------------------------------- */

static int read_native_sector(const dev_node_t *n, uint32_t lba, void *sec)
{
    if (n->kind == DEV_CDROM)
        return ide_read_atapi_sectors(n->drive, lba, 1, sec);
    return ide_read_sectors(n->drive, lba, 1, sec);
}

static int write_native_sector(const dev_node_t *n, uint32_t lba, const void *sec)
{
    /* CD-ROM is read-only; callers are gated by devfs_node_readonly. */
    return ide_write_sectors(n->drive, lba, 1, sec);
}

long devfs_pread(int idx, void *buf, uint32_t len, uint32_t off)
{
    if (idx < 0 || idx >= s_count || !buf) return -1;
    dev_node_t *n = &s_nodes[idx];
    if (n->kind == DEV_TTY) return 0;       /* VT sink: nothing to read back */
    uint32_t ssz   = node_sector_size(n);
    uint32_t total = n->sectors * ssz;

    if (off >= total) return 0;
    if (len > total - off) len = total - off;

    uint8_t   sec[ATAPI_SECTOR];   /* large enough for both sector sizes */
    uint8_t  *out = (uint8_t *)buf;
    uint32_t  done = 0;

    while (done < len) {
        uint32_t abs   = off + done;
        uint32_t lba   = n->base_lba + abs / ssz;
        uint32_t soff  = abs % ssz;
        uint32_t chunk = ssz - soff;
        if (chunk > len - done) chunk = len - done;

        if (read_native_sector(n, lba, sec) != 0)
            return (done > 0) ? (long)done : -1;
        memcpy(out + done, sec + soff, chunk);
        done += chunk;
    }
    return (long)done;
}

long devfs_pwrite(int idx, const void *buf, uint32_t len, uint32_t off)
{
    if (idx < 0 || idx >= s_count || !buf) return -1;
    dev_node_t *n = &s_nodes[idx];
    if (n->kind == DEV_TTY)                 /* route bytes to the VT grid */
        return vtty_write((int)n->drive, (const char *)buf, len);
    if (n->readonly) return -1;

    uint32_t ssz   = node_sector_size(n);
    uint32_t total = n->sectors * ssz;

    if (off >= total) return 0;
    if (len > total - off) len = total - off;

    uint8_t        sec[ATAPI_SECTOR];
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t       done = 0;

    while (done < len) {
        uint32_t abs   = off + done;
        uint32_t lba   = n->base_lba + abs / ssz;
        uint32_t soff  = abs % ssz;
        uint32_t chunk = ssz - soff;
        if (chunk > len - done) chunk = len - done;

        /* Read-modify-write unless we're overwriting a whole sector. */
        if (soff != 0 || chunk != ssz) {
            if (read_native_sector(n, lba, sec) != 0)
                return (done > 0) ? (long)done : -1;
        }
        memcpy(sec + soff, in + done, chunk);
        if (write_native_sector(n, lba, sec) != 0)
            return (done > 0) ? (long)done : -1;
        done += chunk;
    }
    return (long)done;
}

/* ------------------------------------------------------------------------- */

int devfs_ls(const char *path)
{
    if (!path || path[0] != '/') return -1;

    if (path[1] == '\0') {
        for (int i = 0; i < s_count; i++) {
            t_writestring(s_nodes[i].name);
            t_writestring("\n");
        }
        return 0;
    }

    if (devfs_lookup(path) >= 0) {
        t_writestring("ls: " DEVFS_MOUNT);
        t_writestring(path);
        t_writestring(": Not a directory\n");
        return -1;
    }
    t_writestring("ls: " DEVFS_MOUNT);
    t_writestring(path);
    t_writestring(": No such device\n");
    return -1;
}

int devfs_complete(const char *dir, const char *prefix,
                   fat32_complete_cb_t cb, void *ctx)
{
    (void)dir;  /* /dev is flat */
    if (!cb) return -1;
    size_t plen = prefix ? strlen(prefix) : 0;
    for (int i = 0; i < s_count; i++) {
        if (plen == 0 || strncmp(s_nodes[i].name, prefix, plen) == 0)
            cb(s_nodes[i].name, 0, ctx);
    }
    return 0;
}
