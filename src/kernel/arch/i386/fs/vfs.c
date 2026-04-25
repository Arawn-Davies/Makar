/*
 * vfs.c — lightweight Virtual Filesystem routing layer.
 *
 * Path namespace:
 *   /          virtual root (ls shows mount-points)
 *   /hd/…      FAT32 hard-disk partition
 *   /cdrom/…   ISO9660 CD-ROM
 *
 * All VFS paths are absolute after normalisation.  Relative paths are
 * resolved against the current working directory (s_cwd).
 *
 * Path normalisation handles:
 *   - Multiple consecutive '/' characters  → collapsed to one
 *   - '.'  components                      → discarded
 *   - '..' components                      → parent directory
 */

#include <kernel/vfs.h>
#include <kernel/fat32.h>
#include <kernel/iso9660.h>
#include <kernel/ide.h>
#include <kernel/partition.h>
#include <kernel/tty.h>
#include <kernel/heap.h>
#include <string.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

static char s_cwd[VFS_PATH_MAX];   /* current working directory  */
static int  s_cdrom_drive = -1;    /* IDE drive index of CD-ROM, -1 = none */
static uint32_t s_boot_biosdev = 0xFFu; /* BIOS drive we booted from (0xFF = unknown) */

/* -------------------------------------------------------------------------
 * Filesystem identifiers returned by vfs_route()
 * ---------------------------------------------------------------------- */
#define VFS_FS_ROOT    0
#define VFS_FS_HD      1
#define VFS_FS_CDROM   2
#define VFS_FS_UNKNOWN (-1)

/* -------------------------------------------------------------------------
 * path_normalize – canonicalise an absolute path in-place.
 *
 * 'in'  : source path (may be the same buffer as 'out')
 * 'out' : destination buffer of at least outsz bytes
 *
 * The result always starts with '/', never ends with '/' (unless it IS "/"),
 * and contains no '.' or '..' components.
 * ---------------------------------------------------------------------- */
static void path_normalize(const char *in, char *out, int outsz)
{
    /*
     * comp_start[i] records the write position in 'out' at which component i
     * begins (before its leading '/' separator).  Popping a component with '..'
     * restores olen to comp_start[i], which strips the separator too.
     */
    int comp_start[VFS_PATH_MAX / 2];
    int top  = 0;
    int olen = 0;

    /* The output always starts with '/'. */
    if (outsz > 1) out[olen++] = '/';

    const char *p = in;
    if (*p == '/') p++;

    while (*p) {
        /* Extract the next path component. */
        const char *seg  = p;
        while (*p && *p != '/') p++;
        int slen = (int)(p - seg);
        if (*p == '/') p++;

        if (slen == 0 || (slen == 1 && seg[0] == '.'))
            continue;   /* skip empty segments and '.' */

        if (slen == 2 && seg[0] == '.' && seg[1] == '.') {
            /* Go up: restore olen to where the parent component started. */
            if (top > 0)
                olen = comp_start[--top];
            continue;
        }

        /*
         * Record the current olen so a later '..' can undo this component.
         * olen currently points to the character after the previous component
         * (or to '/' + 1 for the root).  Restoring to comp_start[top] will
         * also strip the '/' separator we are about to add, which is correct.
         */
        if (top < VFS_PATH_MAX / 2)
            comp_start[top++] = olen;

        /* Add separator (except immediately after the root '/'). */
        if (olen > 1 && olen < outsz - 1)
            out[olen++] = '/';

        /* Copy the component, respecting the output buffer limit. */
        for (int i = 0; i < slen && olen < outsz - 1; i++)
            out[olen++] = seg[i];
    }

    out[olen] = '\0';
}

/* -------------------------------------------------------------------------
 * path_resolve – resolve 'path' to an absolute VFS path stored in 'out'.
 *
 * Relative paths are joined to s_cwd.  NULL or empty path → CWD.
 * ---------------------------------------------------------------------- */
static void path_resolve(const char *path, char *out)
{
    char tmp[VFS_PATH_MAX * 2];

    if (!path || !*path) {
        path_normalize(s_cwd, out, VFS_PATH_MAX);
        return;
    }

    if (path[0] == '/') {
        path_normalize(path, out, VFS_PATH_MAX);
        return;
    }

    /* Relative: prepend CWD. */
    int clen = (int)strlen(s_cwd);
    int plen = (int)strlen(path);
    /* Need clen + '/' + plen + NUL bytes total. */
    if (clen + 1 + plen + 1 <= (int)sizeof(tmp)) {
        memcpy(tmp, s_cwd, (size_t)clen);
        tmp[clen] = '/';
        memcpy(tmp + clen + 1, path, (size_t)(plen + 1));
    } else {
        tmp[0] = '/'; tmp[1] = '\0';
    }
    path_normalize(tmp, out, VFS_PATH_MAX);
}

/* -------------------------------------------------------------------------
 * vfs_route – determine which driver handles 'abs' and set *drv_path to
 * the driver-relative path (always starts with '/').
 *
 * Returns VFS_FS_ROOT, VFS_FS_HD, VFS_FS_CDROM, or VFS_FS_UNKNOWN.
 * ---------------------------------------------------------------------- */
static int vfs_route(const char *abs, const char **drv_path)
{
    /* Root "/" */
    if (abs[0] == '/' && abs[1] == '\0') {
        *drv_path = "/";
        return VFS_FS_ROOT;
    }

    /* "/hd" or "/hd/…" */
    if (abs[1] == 'h' && abs[2] == 'd' &&
        (abs[3] == '/' || abs[3] == '\0')) {
        *drv_path = (abs[3] == '/') ? (abs + 3) : "/";
        return VFS_FS_HD;
    }

    /* "/cdrom" or "/cdrom/…" */
    if (abs[1] == 'c' && abs[2] == 'd' && abs[3] == 'r' &&
        abs[4] == 'o' && abs[5] == 'm' &&
        (abs[6] == '/' || abs[6] == '\0')) {
        *drv_path = (abs[6] == '/') ? (abs + 6) : "/";
        return VFS_FS_CDROM;
    }

    *drv_path = abs;
    return VFS_FS_UNKNOWN;
}

/* -------------------------------------------------------------------------
 * ls_root – list the virtual root directory.
 * ---------------------------------------------------------------------- */
static void ls_root(void)
{
    if (fat32_mounted())    t_writestring("[hd]\n");
    if (s_cdrom_drive >= 0) t_writestring("[cdrom]\n");
    if (!fat32_mounted() && s_cdrom_drive < 0)
        t_writestring("(no filesystems mounted — use 'mount' to mount FAT32)\n");
}

/* =========================================================================
 * Public API
 * ====================================================================== */

void vfs_init(void)
{
    s_cwd[0] = '/';
    s_cwd[1] = '\0';
    s_cdrom_drive = -1;

    /* Scan IDE bus for an ATAPI drive that contains a valid ISO9660 volume. */
    for (int i = 0; i < IDE_MAX_DRIVES; i++) {
        const ide_drive_t *d = ide_get_drive((uint8_t)i);
        if (!d || !d->present || d->type != IDE_TYPE_ATAPI)
            continue;
        if (iso9660_probe((uint8_t)i) == 0) {
            s_cdrom_drive = i;
            break;
        }
    }
}

const char *vfs_getcwd(void)
{
    return s_cwd;
}

void vfs_notify_hd_mounted(void)
{
    if (strcmp(s_cwd, "/") == 0)
        memcpy(s_cwd, "/hd", 4);  /* includes NUL */
}

void vfs_notify_hd_unmounted(void)
{
    /* Reset CWD to "/" if it was somewhere under "/hd". */
    if (s_cwd[1] == 'h' && s_cwd[2] == 'd' &&
        (s_cwd[3] == '/' || s_cwd[3] == '\0')) {
        s_cwd[0] = '/';
        s_cwd[1] = '\0';
    }
}

void vfs_notify_cdrom_ejected(void)
{
    /* Mark the CD-ROM as gone so /cdrom paths are no longer served. */
    s_cdrom_drive = -1;

    /* Reset CWD to "/" if it was somewhere under "/cdrom". */
    if (s_cwd[1] == 'c' && s_cwd[2] == 'd' && s_cwd[3] == 'r' &&
        s_cwd[4] == 'o' && s_cwd[5] == 'm' &&
        (s_cwd[6] == '/' || s_cwd[6] == '\0')) {
        s_cwd[0] = '/';
        s_cwd[1] = '\0';
    }
}

/* -------------------------------------------------------------------------
 * vfs_set_boot_drive / vfs_auto_mount
 * ---------------------------------------------------------------------- */

void vfs_set_boot_drive(uint32_t biosdev)
{
    s_boot_biosdev = biosdev;
}

/* Static partition table used by vfs_auto_mount (avoids large stack alloc). */
static disk_parts_t s_auto_parts;

/*
 * try_mount_hdd – probe 'drive' for a FAT32 partition and mount the first one.
 * Returns 1 on success, 0 on failure.
 */
static int try_mount_hdd(uint8_t drive)
{
    const ide_drive_t *d = ide_get_drive(drive);
    if (!d || !d->present || d->type != IDE_TYPE_ATA)
        return 0;

    if (part_probe(drive, &s_auto_parts) != 0)
        return 0;

    for (int i = 0; i < s_auto_parts.count; i++) {
        const part_info_t *p = &s_auto_parts.parts[i];
        int is_fat32 = 0;

        if (s_auto_parts.scheme == PART_SCHEME_MBR) {
            is_fat32 = (p->mbr_type == PART_MBR_FAT32_CHS ||
                        p->mbr_type == PART_MBR_FAT32_LBA);
        } else if (s_auto_parts.scheme == PART_SCHEME_GPT) {
            is_fat32 = (memcmp(p->type_guid, PART_GUID_FAT32, 16) == 0);
        }

        if (!is_fat32)
            continue;

        if (fat32_mount(drive, p->lba_start) == 0) {
            vfs_notify_hd_mounted();
            t_writestring("Auto-mounted FAT32 (drive ");
            t_dec(drive);
            t_writestring(", partition ");
            t_dec((uint32_t)(i + 1));
            t_writestring(") at /hd\n");
            return 1;
        }
    }
    return 0;
}

void vfs_auto_mount(void)
{
    /*
     * Always probe every ATA drive and mount the first FAT32 partition
     * found, regardless of the boot device.  This ensures the HDD is
     * mounted even when the system is booted from the CD-ROM.
     *
     * The BIOS boot-device number (set by vfs_set_boot_drive) is used only
     * as a hint: if it points to a specific ATA drive, try that drive first
     * so the most likely partition is mounted with priority.
     */
    int hd_mounted = 0;

    /* If biosdev identifies a specific HDD, try it first. */
    if (s_boot_biosdev >= 0x80u && s_boot_biosdev <= 0xDFu) {
        uint8_t hint_drive = (uint8_t)(s_boot_biosdev - 0x80u);
        hd_mounted = try_mount_hdd(hint_drive);
    }

    /* Scan remaining drives in order (skip the hint drive if already tried). */
    for (int i = 0; i < IDE_MAX_DRIVES && !hd_mounted; i++) {
        /* Skip the hint drive — already tried above. */
        if (s_boot_biosdev >= 0x80u && s_boot_biosdev <= 0xDFu &&
            (uint8_t)i == (uint8_t)(s_boot_biosdev - 0x80u))
            continue;

        hd_mounted = try_mount_hdd((uint8_t)i);
    }

    /* Report CD-ROM status (always registered by vfs_init if present). */
    if (s_cdrom_drive >= 0) {
        t_writestring("CD-ROM detected, accessible at /cdrom\n");
        /* If no HDD was mounted, navigate CWD to /cdrom. */
        if (!hd_mounted)
            memcpy(s_cwd, "/cdrom", 7);   /* 7 includes the NUL terminator */
    }

    if (!hd_mounted && s_cdrom_drive < 0) {
        /* Nothing mounted — leave CWD at "/" and let the user mount manually. */
    }
}

/* -------------------------------------------------------------------------
 * vfs_ls
 * ---------------------------------------------------------------------- */
int vfs_ls(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);

    const char *drv;
    switch (vfs_route(abs, &drv)) {
    case VFS_FS_ROOT:
        ls_root();
        return 0;

    case VFS_FS_HD:
        if (!fat32_mounted()) {
            t_writestring("ls: /hd is not mounted (use: mount <drv> <part>)\n");
            return -1;
        }
        return fat32_ls(drv);

    case VFS_FS_CDROM:
        if (s_cdrom_drive < 0) {
            t_writestring("ls: /cdrom — no ISO9660 CD-ROM detected\n");
            return -1;
        }
        return iso9660_ls((uint8_t)s_cdrom_drive, drv);

    default:
        t_writestring("ls: path not found\n");
        return -1;
    }
}

/* -------------------------------------------------------------------------
 * vfs_cd
 * ---------------------------------------------------------------------- */
int vfs_cd(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);

    const char *drv;
    int fs = vfs_route(abs, &drv);

    switch (fs) {
    case VFS_FS_ROOT:
        /* Always valid. */
        memcpy(s_cwd, abs, (size_t)(strlen(abs) + 1u));
        return 0;

    case VFS_FS_HD:
        if (!fat32_mounted()) {
            t_writestring("cd: /hd is not mounted\n");
            return -1;
        }
        /* Use fat32_cd for validation; it updates FAT32's internal CWD too. */
        if (fat32_cd(drv) != 0) {
            t_writestring("cd: directory not found\n");
            return -1;
        }
        strncpy(s_cwd, abs, VFS_PATH_MAX - 1);
        s_cwd[VFS_PATH_MAX - 1] = '\0';
        return 0;

    case VFS_FS_CDROM:
        if (s_cdrom_drive < 0) {
            t_writestring("cd: /cdrom — no ISO9660 CD-ROM detected\n");
            return -1;
        }
        /* No cheap directory check for ISO9660; optimistically update CWD. */
        strncpy(s_cwd, abs, VFS_PATH_MAX - 1);
        s_cwd[VFS_PATH_MAX - 1] = '\0';
        return 0;

    default:
        t_writestring("cd: path not found\n");
        return -1;
    }
}

/* -------------------------------------------------------------------------
 * vfs_cat – read and print a file to the terminal (up to 64 KiB).
 * ---------------------------------------------------------------------- */
int vfs_cat(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);

    const char *drv;
    int fs = vfs_route(abs, &drv);

    enum { CAT_MAX = 64u * 1024u };
    uint8_t *buf = (uint8_t *)kmalloc(CAT_MAX);
    if (!buf) {
        t_writestring("cat: out of memory\n");
        return -1;
    }

    uint32_t got = 0;
    int err;

    switch (fs) {
    case VFS_FS_HD:
        if (!fat32_mounted()) {
            t_writestring("cat: /hd is not mounted\n");
            kfree(buf);
            return -1;
        }
        err = fat32_read_file(drv, buf, CAT_MAX, &got);
        break;

    case VFS_FS_CDROM:
        if (s_cdrom_drive < 0) {
            t_writestring("cat: /cdrom — no ISO9660 CD-ROM detected\n");
            kfree(buf);
            return -1;
        }
        err = iso9660_read_file((uint8_t)s_cdrom_drive, drv, buf, CAT_MAX, &got);
        break;

    default:
        t_writestring("cat: not a file\n");
        kfree(buf);
        return -1;
    }

    if (err) {
        t_writestring("cat: file not found\n");
        kfree(buf);
        return -1;
    }

    t_write((const char *)buf, got);
    if (got > 0 && buf[got - 1] != '\n')
        t_putchar('\n');

    kfree(buf);
    return 0;
}

/* -------------------------------------------------------------------------
 * vfs_mkdir
 * ---------------------------------------------------------------------- */
int vfs_mkdir(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);

    const char *drv;
    if (vfs_route(abs, &drv) != VFS_FS_HD) {
        t_writestring("mkdir: only supported under /hd\n");
        return -1;
    }
    if (!fat32_mounted()) {
        t_writestring("mkdir: /hd is not mounted\n");
        return -1;
    }
    return fat32_mkdir(drv);
}

/* -------------------------------------------------------------------------
 * vfs_read_file / vfs_write_file
 * ---------------------------------------------------------------------- */
int vfs_read_file(const char *path, void *buf, uint32_t bufsz, uint32_t *out_sz)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);

    const char *drv;
    switch (vfs_route(abs, &drv)) {
    case VFS_FS_HD:
        if (!fat32_mounted()) return -1;
        return fat32_read_file(drv, buf, bufsz, out_sz);

    case VFS_FS_CDROM:
        if (s_cdrom_drive < 0) return -1;
        return iso9660_read_file((uint8_t)s_cdrom_drive, drv, buf, bufsz, out_sz);

    default:
        return -1;
    }
}

int vfs_write_file(const char *path, const void *buf, uint32_t size)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);

    const char *drv;
    if (vfs_route(abs, &drv) != VFS_FS_HD) return -1;
    if (!fat32_mounted()) return -1;
    return fat32_write_file(drv, buf, size);
}
