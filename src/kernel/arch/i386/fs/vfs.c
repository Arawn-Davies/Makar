/*
 * vfs.c - lightweight Virtual Filesystem routing layer.
 *
 * Path namespace:
 *   /          virtual root (ls shows mount-points)
 *   /hd/…      FAT32 hard-disk partition
 *   /cdrom/…   ISO9660 CD-ROM
 *
 * All VFS paths are absolute after normalisation.  Relative paths are
 * resolved against the calling task's cwd (task_current()->cwd).
 *
 * During boot (before tasking_init), there is no task_current().  Writers
 * fall back to s_boot_cwd, which is then handed off to idle->cwd inside
 * tasking_init via vfs_getcwd().  Post-tasking, every cwd read/write is
 * per-task, so VT0 may sit in /hd/apps while VT1 sits in /cdrom/boot
 * without cross-contamination.
 *
 * Path normalisation handles:
 *   - Multiple consecutive '/' characters  → collapsed to one
 *   - '.'  components                      → discarded
 *   - '..' components                      → parent directory
 */

#include <kernel/vfs.h>
#include <kernel/fat32.h>
#include <kernel/iso9660.h>
#include <kernel/procfs.h>
#include <kernel/devfs.h>
#include <kernel/ide.h>
#include <kernel/partition.h>
#include <kernel/tty.h>
#include <kernel/heap.h>
#include <kernel/task.h>
#include <string.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

/*
 * Pre-tasking-init scratch cwd.  vfs_init() / vfs_auto_mount() run before
 * tasking_init() and need somewhere to record the initial cwd.  Once idle
 * exists, cwd_buf() switches to task_current()->cwd and this buffer becomes
 * the source for the one-time handoff inside tasking_init.
 */
static char s_boot_cwd[VFS_PATH_MAX] = "/";
static int  s_cdrom_drive = -1;    /* IDE drive index of CD-ROM, -1 = none */

/* Component name under /mnt where the single FAT32 volume is mounted.
 * Default "hd" (the OS drive); `mount /dev/hdaN /mnt/<name>` changes it. */
#define VFS_MOUNT_NAME_MAX 32
static char s_hd_mount[VFS_MOUNT_NAME_MAX] = "hd";
static uint32_t s_boot_biosdev = 0xFFu; /* BIOS drive we booted from (0xFF = unknown) */

/* Resolve the cwd backing store for the calling context.  Pre-tasking
 * (vfs_init, vfs_auto_mount) returns the boot scratch buffer; once tasking
 * is up every reader and writer hits the calling task's own cwd field. */
static char *cwd_buf(void)
{
    task_t *t = task_current();
    return t ? t->cwd : s_boot_cwd;
}

/* -------------------------------------------------------------------------
 * Filesystem identifiers returned by vfs_route()
 * ---------------------------------------------------------------------- */
#define VFS_FS_ROOT    0
#define VFS_FS_HD      1
#define VFS_FS_CDROM   2
#define VFS_FS_PROC    3
#define VFS_FS_DEV     4
#define VFS_FS_MNT     5
#define VFS_FS_UNKNOWN (-1)

/* Mount-point prefix for disk filesystems.  /mnt/hd and /mnt/cdrom are
 * canonical; bare /hd and /cdrom are transitional aliases (see vfs_route). */
#define VFS_MNT      "/mnt"
#define VFS_MNT_LEN  4

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
 * Relative paths are joined to the calling task's cwd (or the boot scratch
 * cwd if invoked before tasking_init).  NULL or empty path → CWD.
 * ---------------------------------------------------------------------- */
static void path_resolve(const char *path, char *out)
{
    char tmp[VFS_PATH_MAX * 2];
    const char *cwd = cwd_buf();

    if (!path || !*path) {
        path_normalize(cwd, out, VFS_PATH_MAX);
        return;
    }

    if (path[0] == '/') {
        path_normalize(path, out, VFS_PATH_MAX);
        return;
    }

    /* Relative: prepend CWD. */
    int clen = (int)strlen(cwd);
    int plen = (int)strlen(path);
    /* Need clen + '/' + plen + NUL bytes total. */
    if (clen + 1 + plen + 1 <= (int)sizeof(tmp)) {
        memcpy(tmp, cwd, (size_t)clen);
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

    /* Disk filesystems live under /mnt (Linux convention).  The FAT32
     * volume mounts at a caller-chosen component under /mnt (default
     * "hd", the OS drive); the CD-ROM is fixed at /mnt/cdrom.  A bare
     * "/hd" / "/cdrom" remains a transitional alias to the same driver.
     *
     * Under /mnt we split off the first path component and match it
     * against the live mountpoints; the remainder becomes the
     * driver-relative path. */
    if (memcmp(abs, VFS_MNT, VFS_MNT_LEN) == 0 &&
        (abs[VFS_MNT_LEN] == '/' || abs[VFS_MNT_LEN] == '\0')) {
        if (abs[VFS_MNT_LEN] == '\0') {
            *drv_path = "/";
            return VFS_FS_MNT;
        }
        const char *comp = abs + VFS_MNT_LEN + 1;   /* after "/mnt/"      */
        const char *rest = comp;
        while (*rest && *rest != '/') rest++;        /* end of component   */
        size_t clen = (size_t)(rest - comp);
        const char *dp = (*rest == '/') ? rest : "/";

        if (clen == 5 && memcmp(comp, "cdrom", 5) == 0) {
            *drv_path = dp;
            return VFS_FS_CDROM;
        }
        if (clen == strlen(s_hd_mount) &&
            memcmp(comp, s_hd_mount, clen) == 0) {
            *drv_path = dp;
            return VFS_FS_HD;
        }
        *drv_path = abs;
        return VFS_FS_UNKNOWN;
    }

    /* Bare "/hd" / "/cdrom" aliases (resolve to the same driver). */
    if (abs[1] == 'h' && abs[2] == 'd' &&
        (abs[3] == '/' || abs[3] == '\0')) {
        *drv_path = (abs[3] == '/') ? (abs + 3) : "/";
        return VFS_FS_HD;
    }
    if (abs[1] == 'c' && abs[2] == 'd' && abs[3] == 'r' &&
        abs[4] == 'o' && abs[5] == 'm' &&
        (abs[6] == '/' || abs[6] == '\0')) {
        *drv_path = (abs[6] == '/') ? (abs + 6) : "/";
        return VFS_FS_CDROM;
    }

    /* /proc (mount prefix is the single source of truth in procfs.h). */
    if (memcmp(abs, PROCFS_MOUNT, PROCFS_MOUNT_LEN) == 0 &&
        (abs[PROCFS_MOUNT_LEN] == '/' || abs[PROCFS_MOUNT_LEN] == '\0')) {
        *drv_path = (abs[PROCFS_MOUNT_LEN] == '/')
                        ? (abs + PROCFS_MOUNT_LEN)
                        : "/";
        return VFS_FS_PROC;
    }

    /* /dev (synthetic block-device tree). */
    if (memcmp(abs, DEVFS_MOUNT, DEVFS_MOUNT_LEN) == 0 &&
        (abs[DEVFS_MOUNT_LEN] == '/' || abs[DEVFS_MOUNT_LEN] == '\0')) {
        *drv_path = (abs[DEVFS_MOUNT_LEN] == '/')
                        ? (abs + DEVFS_MOUNT_LEN)
                        : "/";
        return VFS_FS_DEV;
    }

    *drv_path = abs;
    return VFS_FS_UNKNOWN;
}

/* -------------------------------------------------------------------------
 * ls_root – list the virtual root directory.
 * ---------------------------------------------------------------------- */
static void ls_root(void)
{
    /* Disk filesystems now live under /mnt; the root shows the synthetic
     * trees plus the /mnt container. */
    t_writestring("[mnt]\n");    /* hd / cdrom live here                 */
    t_writestring("[proc]\n");   /* always present - synthesised         */
    t_writestring("[dev]\n");    /* always present - synthesised         */
}

/* List /mnt - the disk-filesystem mount container. */
static void ls_mnt(void)
{
    if (fat32_mounted()) {
        t_putchar('[');
        t_writestring(s_hd_mount);
        t_writestring("]\n");
    }
    if (s_cdrom_drive >= 0) t_writestring("[cdrom]\n");
    if (!fat32_mounted() && s_cdrom_drive < 0)
        t_writestring("(no disk filesystems mounted - use 'mount' to mount FAT32)\n");
}

/* =========================================================================
 * Public API
 * ====================================================================== */

void vfs_init(void)
{
    s_boot_cwd[0] = '/';
    s_boot_cwd[1] = '\0';
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

    /* Build the /dev node table from the just-scanned IDE bus. */
    devfs_init();
}

const char *vfs_getcwd(void)
{
    return cwd_buf();
}

/* Set the /mnt component the FAT32 volume is reachable at.  `name` is a
 * single component (no slashes); NULL/empty resets to the default "hd".
 * Rejected silently if it contains a '/' or overflows the buffer. */
void vfs_set_hd_mount(const char *name)
{
    if (!name || !*name) {
        memcpy(s_hd_mount, "hd", 3);
        return;
    }
    for (const char *q = name; *q; q++)
        if (*q == '/') return;
    if (strlen(name) >= VFS_MOUNT_NAME_MAX) return;
    strncpy(s_hd_mount, name, VFS_MOUNT_NAME_MAX - 1);
    s_hd_mount[VFS_MOUNT_NAME_MAX - 1] = '\0';
}

const char *vfs_hd_mount(void)
{
    return s_hd_mount;
}

/*
 * vfs_notify_hd_mounted / _unmounted / _cdrom_ejected
 *
 * Mount-state transitions can leave individual tasks parked under a mount
 * point that just disappeared (or, for the hd-mounted case, sitting on "/"
 * when /hd just became browsable).  Walk every live task and fix up each
 * one's cwd independently - using cwd_buf() here would only mutate the
 * caller's cwd, which is rarely the task that needs the adjustment.
 *
 * Pre-tasking-init this loop is a no-op (task_get returns NULL), and
 * s_boot_cwd is rewritten directly so the same rules apply during the
 * vfs_init / vfs_auto_mount window.
 */
/* Compose the canonical mount path "/mnt/<name>" into a small buffer. */
static void hd_mount_path(char *out, size_t outsz)
{
    /* outsz is always >= sizeof("/mnt/") + VFS_MOUNT_NAME_MAX here. */
    int n = 0;
    const char *pre = "/mnt/";
    while (pre[n] && n < (int)outsz - 1) { out[n] = pre[n]; n++; }
    for (int i = 0; s_hd_mount[i] && n < (int)outsz - 1; i++)
        out[n++] = s_hd_mount[i];
    out[n] = '\0';
}

static void fixup_cwd_hd_mounted(char *cwd)
{
    if (strcmp(cwd, "/") == 0) {
        char mp[VFS_MOUNT_NAME_MAX + 8];
        hd_mount_path(mp, sizeof(mp));
        memcpy(cwd, mp, strlen(mp) + 1);
    }
}

static void fixup_cwd_hd_unmounted(char *cwd)
{
    /* Match the canonical /mnt/<name> as well as the bare /hd alias. */
    char mp[VFS_MOUNT_NAME_MAX + 8];
    hd_mount_path(mp, sizeof(mp));
    size_t mlen = strlen(mp);
    if (strcmp(cwd, mp) == 0 ||
        (strncmp(cwd, mp, mlen) == 0 && cwd[mlen] == '/') ||
        (cwd[1] == 'h' && cwd[2] == 'd' && (cwd[3] == '/' || cwd[3] == '\0'))) {
        cwd[0] = '/';
        cwd[1] = '\0';
    }
}

static void fixup_cwd_cdrom_ejected(char *cwd)
{
    if (strcmp(cwd, "/mnt/cdrom") == 0 || strncmp(cwd, "/mnt/cdrom/", 11) == 0 ||
        (cwd[1] == 'c' && cwd[2] == 'd' && cwd[3] == 'r' &&
         cwd[4] == 'o' && cwd[5] == 'm' &&
         (cwd[6] == '/' || cwd[6] == '\0'))) {
        cwd[0] = '/';
        cwd[1] = '\0';
    }
}

typedef void (*cwd_fixup_fn)(char *);

static void apply_cwd_fixup(cwd_fixup_fn fn)
{
    fn(s_boot_cwd);
    for (int i = 0; ; i++) {
        task_t *t = task_get(i);
        if (!t) break;
        fn(t->cwd);
    }
}

void vfs_notify_hd_mounted(void)   { apply_cwd_fixup(fixup_cwd_hd_mounted); }
void vfs_notify_hd_unmounted(void) { apply_cwd_fixup(fixup_cwd_hd_unmounted); }

void vfs_notify_cdrom_ejected(void)
{
    s_cdrom_drive = -1;
    apply_cwd_fixup(fixup_cwd_cdrom_ejected);
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
            t_writestring(") at /mnt/hd\n");
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
     * as a priority hint: if biosdev is in the HDD range (0x80–0xDF), the
     * corresponding IDE index (biosdev − 0x80) is tried first.  The full
     * scan that follows is always exhaustive - it does NOT skip the hint
     * drive even if the hint already failed.
     *
     * Why exhaustive?  The biosdev → IDE-index mapping is not guaranteed
     * 1-to-1.  GRUB reports the BIOS device of the root device (where the
     * kernel file was found), not necessarily the drive that boot.img ran
     * from.  If GRUB's embedded search picked up the ISO CD-ROM's grub.cfg
     * first (because it was enumerated before the HDD's FAT32 partition),
     * the reported biosdev is the CD-ROM's BIOS device, whose index
     * (biosdev − 0x80) maps to a non-existent IDE slot.  The hint fails,
     * and the exhaustive scan then finds the real HDD.  Skipping the hint
     * drive in the scan would be harmless in the common case but could hide
     * the HDD when biosdev happens to equal the HDD's BIOS device yet
     * fat32_mount fails on the first try due to a transient IDE condition.
     */
    int hd_mounted = 0;

    /* Priority hint: try the drive that biosdev points to first. */
    if (s_boot_biosdev >= 0x80u && s_boot_biosdev <= 0xDFu) {
        uint8_t hint_drive = (uint8_t)(s_boot_biosdev - 0x80u);
        if (hint_drive < IDE_MAX_DRIVES)
            hd_mounted = try_mount_hdd(hint_drive);
    }

    /* Exhaustive scan: try every slot in order. */
    for (int i = 0; i < IDE_MAX_DRIVES && !hd_mounted; i++)
        hd_mounted = try_mount_hdd((uint8_t)i);

    /* Report CD-ROM status (always registered by vfs_init if present). */
    if (s_cdrom_drive >= 0) {
        t_writestring("CD-ROM detected, accessible at /mnt/cdrom\n");
        /* If no HDD was mounted, navigate CWD to /mnt/cdrom.  Pre-tasking,
         * this lands in s_boot_cwd; tasking_init seeds idle->cwd from it. */
        if (!hd_mounted)
            memcpy(cwd_buf(), "/mnt/cdrom", 11);   /* 11 includes NUL */
    }

    if (!hd_mounted && s_cdrom_drive < 0) {
        /* Nothing mounted - leave CWD at "/" and let the user mount manually. */
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

    case VFS_FS_MNT:
        ls_mnt();
        return 0;

    case VFS_FS_HD:
        if (!fat32_mounted()) {
            t_writestring("ls: /mnt/hd is not mounted (use: mount <drv> <part>)\n");
            return -1;
        }
        return fat32_ls(drv);

    case VFS_FS_CDROM:
        if (s_cdrom_drive < 0) {
            t_writestring("ls: /mnt/cdrom - no ISO9660 CD-ROM detected\n");
            return -1;
        }
        return iso9660_ls((uint8_t)s_cdrom_drive, drv);

    case VFS_FS_PROC:
        return procfs_ls(drv);

    case VFS_FS_DEV:
        return devfs_ls(drv);

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
    char *cwd = cwd_buf();

    switch (fs) {
    case VFS_FS_ROOT:
    case VFS_FS_MNT:
        /* Root and the /mnt container are always valid directories. */
        memcpy(cwd, abs, (size_t)(strlen(abs) + 1u));
        return 0;

    case VFS_FS_HD:
        if (!fat32_mounted()) {
            t_writestring("cd: /mnt/hd is not mounted\n");
            return -1;
        }
        /* Use fat32_cd for validation; it updates FAT32's internal CWD too. */
        if (fat32_cd(drv) != 0) {
            t_writestring("cd: directory not found\n");
            return -1;
        }
        strncpy(cwd, abs, VFS_PATH_MAX - 1);
        cwd[VFS_PATH_MAX - 1] = '\0';
        return 0;

    case VFS_FS_CDROM:
        if (s_cdrom_drive < 0) {
            t_writestring("cd: /mnt/cdrom - no ISO9660 CD-ROM detected\n");
            return -1;
        }
        /* No cheap directory check for ISO9660; optimistically update CWD. */
        strncpy(cwd, abs, VFS_PATH_MAX - 1);
        cwd[VFS_PATH_MAX - 1] = '\0';
        return 0;

    case VFS_FS_PROC:
        /* /proc is flat: only "/proc" itself is a directory.  Reject
         * any deeper cd. */
        if (drv[0] == '/' && drv[1] == '\0') {
            strncpy(cwd, abs, VFS_PATH_MAX - 1);
            cwd[VFS_PATH_MAX - 1] = '\0';
            return 0;
        }
        t_writestring("cd: not a directory\n");
        return -1;

    case VFS_FS_DEV:
        /* /dev is flat: only "/dev" itself is a directory. */
        if (drv[0] == '/' && drv[1] == '\0') {
            strncpy(cwd, abs, VFS_PATH_MAX - 1);
            cwd[VFS_PATH_MAX - 1] = '\0';
            return 0;
        }
        t_writestring("cd: not a directory\n");
        return -1;

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
            t_writestring("cat: /mnt/hd is not mounted\n");
            kfree(buf);
            return -1;
        }
        err = fat32_read_file(drv, buf, CAT_MAX, &got);
        break;

    case VFS_FS_CDROM:
        if (s_cdrom_drive < 0) {
            t_writestring("cat: /mnt/cdrom - no ISO9660 CD-ROM detected\n");
            kfree(buf);
            return -1;
        }
        err = iso9660_read_file((uint8_t)s_cdrom_drive, drv, buf, CAT_MAX, &got);
        break;

    case VFS_FS_PROC:
        err = procfs_read_file(drv, buf, CAT_MAX, &got);
        break;

    case VFS_FS_DEV: {
        int idx = devfs_lookup(drv);
        if (idx < 0) {
            t_writestring("cat: no such device\n");
            kfree(buf);
            return -1;
        }
        long r = devfs_pread(idx, buf, CAT_MAX, 0);
        if (r < 0) { err = -1; } else { got = (uint32_t)r; err = 0; }
        break;
    }

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
        t_writestring("mkdir: only supported under /mnt/hd\n");
        return -1;
    }
    if (!fat32_mounted()) {
        t_writestring("mkdir: /mnt/hd is not mounted\n");
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

    case VFS_FS_PROC:
        return procfs_read_file(drv, buf, bufsz, out_sz);

    case VFS_FS_DEV: {
        int idx = devfs_lookup(drv);
        if (idx < 0) return -1;
        long r = devfs_pread(idx, buf, bufsz, 0);
        if (r < 0) return -1;
        if (out_sz) *out_sz = (uint32_t)r;
        return 0;
    }

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

int vfs_delete_file(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);

    const char *drv;
    if (vfs_route(abs, &drv) != VFS_FS_HD) return -1;
    if (!fat32_mounted()) return -1;
    return fat32_delete_file(drv);
}

int vfs_delete_dir(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);

    const char *drv;
    if (vfs_route(abs, &drv) != VFS_FS_HD) return -1;
    if (!fat32_mounted()) return -1;
    return fat32_delete_dir(drv);
}

int vfs_rename(const char *old_path, const char *new_path)
{
    char old_abs[VFS_PATH_MAX], new_abs[VFS_PATH_MAX];
    path_resolve(old_path, old_abs);
    path_resolve(new_path, new_abs);

    const char *old_drv, *new_drv;
    if (vfs_route(old_abs, &old_drv) != VFS_FS_HD) return -1;
    if (vfs_route(new_abs, &new_drv) != VFS_FS_HD) return -1;
    if (!fat32_mounted()) return -1;

    /* Check if source is a file or directory, then call appropriate rename. */
    if (fat32_file_exists(old_drv))
        return fat32_rename_file(old_drv, new_drv);
    return fat32_rename_dir(old_drv, new_drv);
}

int vfs_file_exists(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);

    const char *drv;
    switch (vfs_route(abs, &drv)) {
    case VFS_FS_HD:
        if (!fat32_mounted()) return 0;
        return fat32_file_exists(drv);
    case VFS_FS_CDROM:
        if (s_cdrom_drive < 0) return 0;
        return iso9660_file_exists((uint8_t)s_cdrom_drive, drv);
    case VFS_FS_PROC:
        return procfs_file_exists(drv);
    case VFS_FS_DEV:
        return devfs_file_exists(drv);
    default:
        return 0;
    }
}

int vfs_blockdev_lookup(const char *path, uint32_t *size_out)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);

    const char *drv;
    if (vfs_route(abs, &drv) != VFS_FS_DEV) return -1;
    int idx = devfs_lookup(drv);
    if (idx < 0) return -1;
    if (size_out) *size_out = devfs_node_size(idx);
    return idx;
}

long vfs_blockdev_pread(int node, void *buf, uint32_t len, uint32_t off)
{
    return devfs_pread(node, buf, len, off);
}

long vfs_blockdev_pwrite(int node, const void *buf, uint32_t len, uint32_t off)
{
    return devfs_pwrite(node, buf, len, off);
}

/* -------------------------------------------------------------------------
 * vfs_complete - enumerate directory entries for tab completion.
 *
 * dir    : VFS directory path to enumerate (NULL → use CWD).
 * prefix : passed through to the callback context (caller filters by it).
 * cb     : invoked for each entry found.
 * ctx    : opaque pointer forwarded to cb.
 *
 * Returns 0 on success, -1 if the path is not under /hd or not mounted.
 * ---------------------------------------------------------------------- */
int vfs_complete(const char *dir, const char *prefix,
                 fat32_complete_cb_t cb, void *ctx)
{
    char abs[VFS_PATH_MAX];
    path_resolve(dir ? dir : cwd_buf(), abs);

    const char *drv;
    switch (vfs_route(abs, &drv)) {
    case VFS_FS_ROOT: {
        /* Root holds the synthetic trees plus the /mnt disk container.
         * Mirrors ls_root(). */
        if (cb) {
            cb("mnt",   1, ctx);
            cb("proc",  1, ctx);
            cb("dev",   1, ctx);
        }
        return 0;
    }
    case VFS_FS_MNT: {
        /* /mnt enumerates the live disk filesystems (Mirrors ls_mnt()). */
        if (cb) {
            if (fat32_mounted())    cb("hd",    1, ctx);
            if (s_cdrom_drive >= 0) cb("cdrom", 1, ctx);
        }
        return 0;
    }
    case VFS_FS_HD:
        if (!fat32_mounted()) return -1;
        return fat32_complete(drv, prefix, cb, ctx);
    case VFS_FS_CDROM:
        if (s_cdrom_drive < 0) return -1;
        return iso9660_complete((uint8_t)s_cdrom_drive, drv, prefix, cb, ctx);
    case VFS_FS_PROC:
        return procfs_complete(drv, prefix, cb, ctx);
    case VFS_FS_DEV:
        return devfs_complete(drv, prefix, cb, ctx);
    default:
        return -1;
    }
}
