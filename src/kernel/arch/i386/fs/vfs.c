/*
 * vfs.c - lightweight Virtual Filesystem routing layer.
 *
 * Path namespace:
 *   /              virtual root (ls shows mount-points)
 *   /mnt/hd/…      FAT32 hard-disk partition (mountpoint name configurable)
 *   /mnt/cdrom/…   ISO9660 CD-ROM
 *
 * All VFS paths are absolute after normalisation.  Relative paths are
 * resolved against the calling task's cwd (task_current()->cwd).
 *
 * During boot (before tasking_init), there is no task_current().  Writers
 * fall back to s_boot_cwd, which is then handed off to idle->cwd inside
 * tasking_init via vfs_getcwd().  Post-tasking, every cwd read/write is
 * per-task, so VT0 may sit in /mnt/hd/apps while VT1 sits in /mnt/cdrom/boot
 * without cross-contamination.
 *
 * Path normalisation handles:
 *   - Multiple consecutive '/' characters  → collapsed to one
 *   - '.'  components                      → discarded
 *   - '..' components                      → parent directory
 */

#include <kernel/vfs.h>
#include <kernel/fat32.h>
#include <kernel/ext2.h>
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

/* Max length of a /mnt/<name> mountpoint component (see s_mounts below). */
#define VFS_MOUNT_NAME_MAX 32
static uint32_t s_boot_biosdev = 0xFFu; /* BIOS drive we booted from (0xFF = unknown) */

/* Hard-disk mount table.  Each entry maps a /mnt/<name> mountpoint to a
 * backend filesystem.  FAT32 (kernel + bootloader modules + root, EFI-style)
 * and ext2 (apps / user directories) coexist at separate mountpoints.  The
 * single-volume drivers mean at most one FAT32 + one ext2 are active at once
 * (one per backend); the table enforces that. */
#define HD_FS_NONE   0
#define HD_FS_FAT32  1
#define HD_FS_EXT2   2
#define MAX_HD_MOUNTS 8

/* A mountpoint with fs == HD_FS_NONE is an *empty* mountpoint: a directory
 * created under /mnt (via `mkdir /mnt/<name>`) that has no filesystem bound
 * yet, exactly like a bare mountpoint dir on Linux.  `mount` binds a backend
 * into an existing empty mountpoint (it never creates one); `umount` reverts
 * it back to empty; `rmdir` removes the (empty) mountpoint. */
typedef struct { char name[VFS_MOUNT_NAME_MAX]; int fs; } hd_mount_t;
static hd_mount_t s_mounts[MAX_HD_MOUNTS];
static int        s_nmounts;

/* Find the mount whose name matches comp[0..clen).  Returns index or -1. */
static int hd_find(const char *comp, size_t clen)
{
    for (int i = 0; i < s_nmounts; i++)
        if (strlen(s_mounts[i].name) == clen &&
            memcmp(s_mounts[i].name, comp, clen) == 0)
            return i;
    return -1;
}

/* hd_find for a NUL-terminated name. */
static int hd_find_name(const char *name)
{
    return hd_find(name, strlen(name));
}

/* True if a backend already drives a mount (drivers are single-volume). */
static int backend_in_use(int fs)
{
    for (int i = 0; i < s_nmounts; i++) if (s_mounts[i].fs == fs) return 1;
    return 0;
}

/* True if any HD volume is actually bound (empty mountpoints don't count). */
static int hd_mounted(void)
{
    for (int i = 0; i < s_nmounts; i++)
        if (s_mounts[i].fs != HD_FS_NONE) return 1;
    return 0;
}

/* Dispatch HD-volume operations to the backend selected by 'fs'. */
static int hd_ls(int fs, const char *p)        { return (fs == HD_FS_EXT2) ? ext2_ls(p) : fat32_ls(p); }
static int hd_cd(int fs, const char *p)        { return (fs == HD_FS_EXT2) ? ext2_cd(p) : fat32_cd(p); }
static int hd_mkdir(int fs, const char *p)     { return (fs == HD_FS_EXT2) ? ext2_mkdir(p) : fat32_mkdir(p); }
static int hd_read_file(int fs, const char *p, void *b, uint32_t n, uint32_t *o)
                                               { return (fs == HD_FS_EXT2) ? ext2_read_file(p, b, n, o) : fat32_read_file(p, b, n, o); }
static int hd_write_file(int fs, const char *p, const void *b, uint32_t n)
                                               { return (fs == HD_FS_EXT2) ? ext2_write_file(p, b, n) : fat32_write_file(p, b, n); }
static int hd_delete_file(int fs, const char *p){ return (fs == HD_FS_EXT2) ? ext2_delete_file(p) : fat32_delete_file(p); }
static int hd_delete_dir(int fs, const char *p) { return (fs == HD_FS_EXT2) ? ext2_delete_dir(p) : fat32_delete_dir(p); }
static int hd_file_exists(int fs, const char *p){ return (fs == HD_FS_EXT2) ? ext2_file_exists(p) : fat32_file_exists(p); }
static int hd_complete(int fs, const char *d, const char *pre, fat32_complete_cb_t cb, void *ctx)
                                               { return (fs == HD_FS_EXT2) ? ext2_complete(d, pre, cb, ctx) : fat32_complete(d, pre, cb, ctx); }
static const char *hd_fsname(int fs) { return (fs == HD_FS_EXT2) ? "ext2" : (fs == HD_FS_FAT32) ? "FAT32" : "none"; }

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
#define VFS_FS_MNT_EMPTY 6   /* /mnt/<name> placeholder, no fs bound yet */
#define VFS_FS_UNKNOWN (-1)

/* Mount-point prefix for disk filesystems.  Disk volumes live under
 * /mnt: /mnt/boot (FAT32 boot partition), /mnt/root (data partition,
 *       ext2 or FAT32), /mnt/hd (legacy single-partition), and /mnt/cdrom. */
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
static int vfs_route(const char *abs, const char **drv_path, int *out_fs)
{
    if (out_fs) *out_fs = HD_FS_NONE;

    /* Root "/" */
    if (abs[0] == '/' && abs[1] == '\0') {
        *drv_path = "/";
        return VFS_FS_ROOT;
    }

    /* Disk filesystems live under /mnt (Linux convention).  The FAT32
     * volume mounts at a caller-chosen component under /mnt (default
     * "hd", the OS drive); the CD-ROM is fixed at /mnt/cdrom.
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
        int mi = hd_find(comp, clen);
        if (mi >= 0) {
            *drv_path = dp;
            if (s_mounts[mi].fs == HD_FS_NONE)
                return VFS_FS_MNT_EMPTY;   /* placeholder, nothing bound yet */
            if (out_fs) *out_fs = s_mounts[mi].fs;
            return VFS_FS_HD;
        }
        *drv_path = abs;
        return VFS_FS_UNKNOWN;
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
    for (int i = 0; i < s_nmounts; i++) {
        t_putchar('[');
        t_writestring(s_mounts[i].name);
        t_writestring("]");
        if (s_mounts[i].fs == HD_FS_NONE)
            t_writestring("  (empty mountpoint)");
        else {
            t_writestring("  ");
            t_writestring(hd_fsname(s_mounts[i].fs));
        }
        t_putchar('\n');
    }
    if (s_cdrom_drive >= 0) t_writestring("[cdrom]\n");
    if (s_nmounts == 0 && s_cdrom_drive < 0)
        t_writestring("(no mountpoints - use 'mkdir /mnt/<name>' then 'mount')\n");
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

    /* Pre-register the disk mountpoints used by vfs_auto_mount and the
     * installer.  Entries are empty (HD_FS_NONE) until a backend is bound.
     *   /mnt/boot – FAT32 boot partition (kernel + limine stage 3)
     *   /mnt/root – data partition        (apps / docs / src; ext2 or FAT32)
     *   /mnt/hd   – legacy single-partition compatibility mount point        */
    static const char *prebuilt[] = { "boot", "root", "hd" };
    for (int i = 0; i < 3; i++) {
        hd_mount_t *m = &s_mounts[s_nmounts++];
        strncpy(m->name, prebuilt[i], VFS_MOUNT_NAME_MAX - 1);
        m->name[VFS_MOUNT_NAME_MAX - 1] = '\0';
        m->fs = HD_FS_NONE;
    }

    /* Build the /dev node table from the just-scanned IDE bus. */
    devfs_init();
}

const char *vfs_getcwd(void)
{
    return cwd_buf();
}

/*
 * cwd fixups.  Mount-state transitions can leave tasks parked under a mount
 * point that just disappeared (or sitting on "/" when a volume just became
 * browsable).  apply_cwd_fixup walks the boot scratch cwd plus every live
 * task's cwd; the helper reads s_fixup_name for the affected mountpoint.
 */
static const char *s_fixup_name;   /* mountpoint name for the current pass */

static void mount_path_of(const char *name, char *out, size_t outsz)
{
    int n = 0;
    const char *pre = "/mnt/";
    while (pre[n] && n < (int)outsz - 1) { out[n] = pre[n]; n++; }
    for (int i = 0; name[i] && n < (int)outsz - 1; i++) out[n++] = name[i];
    out[n] = '\0';
}

static void fixup_cwd_mounted(char *cwd)
{
    if (strcmp(cwd, "/") == 0) {
        char mp[VFS_MOUNT_NAME_MAX + 8];
        mount_path_of(s_fixup_name, mp, sizeof(mp));
        memcpy(cwd, mp, strlen(mp) + 1);
    }
}

static void fixup_cwd_unmounted(char *cwd)
{
    char mp[VFS_MOUNT_NAME_MAX + 8];
    mount_path_of(s_fixup_name, mp, sizeof(mp));
    size_t mlen = strlen(mp);
    if (strcmp(cwd, mp) == 0 ||
        (strncmp(cwd, mp, mlen) == 0 && cwd[mlen] == '/')) {
        cwd[0] = '/';
        cwd[1] = '\0';
    }
}

static void fixup_cwd_cdrom_ejected(char *cwd)
{
    if (strcmp(cwd, "/mnt/cdrom") == 0 || strncmp(cwd, "/mnt/cdrom/", 11) == 0) {
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

/* Mount the HD volume at (drive, lba) at /mnt/<name>, auto-selecting the
 * backend (ext2 superblock preferred, else FAT32).  Returns 0 and (if
 * out_fs) the chosen backend, or a negative code:
 *   -1  bad name        -10 name already mounted   -11 backend already in use
 *   -13 'cdrom' reserved  -14 no such mountpoint     <0  backend mount error
 *
 * The mountpoint must already exist as an empty mountpoint (created with
 * `mkdir /mnt/<name>`); mount binds a backend into it but never creates one. */
int vfs_mount_hd(uint8_t drive, uint32_t lba, const char *name, int *out_fs)
{
    if (!name || !*name) name = "hd";
    for (const char *q = name; *q; q++) if (*q == '/') return -1;
    if (strlen(name) >= VFS_MOUNT_NAME_MAX) return -1;
    if (strcmp(name, "cdrom") == 0) return -13;

    int mi = hd_find_name(name);
    if (mi < 0)                          return -14;  /* mkdir /mnt/<name> first */
    if (s_mounts[mi].fs != HD_FS_NONE)   return -10;  /* already has a fs bound  */

    int fs = ext2_probe(drive, lba) ? HD_FS_EXT2 : HD_FS_FAT32;
    if (backend_in_use(fs)) return -11;   /* drivers are single-volume */

    int r = (fs == HD_FS_EXT2) ? ext2_mount(drive, lba)
                               : fat32_mount(drive, lba);
    if (r != 0) return r;

    s_mounts[mi].fs = fs;
    s_fixup_name = s_mounts[mi].name;
    apply_cwd_fixup(fixup_cwd_mounted);
    if (out_fs) *out_fs = fs;
    return 0;
}

/* vfs_make_mountpoint - create an empty /mnt/<name> mountpoint (mkdir).
 * Returns 0, -1 bad name, -6 already exists, -12 table full, -13 reserved. */
int vfs_make_mountpoint(const char *name)
{
    if (!name || !*name) return -1;
    for (const char *q = name; *q; q++) if (*q == '/') return -1;
    if (strlen(name) >= VFS_MOUNT_NAME_MAX) return -1;
    if (strcmp(name, "cdrom") == 0) return -13;
    if (hd_find_name(name) >= 0)    return -6;
    if (s_nmounts >= MAX_HD_MOUNTS) return -12;

    hd_mount_t *m = &s_mounts[s_nmounts++];
    strncpy(m->name, name, VFS_MOUNT_NAME_MAX - 1);
    m->name[VFS_MOUNT_NAME_MAX - 1] = '\0';
    m->fs = HD_FS_NONE;
    return 0;
}

/* vfs_remove_mountpoint - remove an empty /mnt/<name> mountpoint (rmdir).
 * Returns 0, -1 no such mountpoint, -16 busy (a filesystem is bound). */
int vfs_remove_mountpoint(const char *name)
{
    int mi = name ? hd_find_name(name) : -1;
    if (mi < 0) return -1;
    if (s_mounts[mi].fs != HD_FS_NONE) return -16;   /* umount first */
    for (int i = mi; i < s_nmounts - 1; i++) s_mounts[i] = s_mounts[i + 1];
    s_nmounts--;
    return 0;
}

/* Unmount the /mnt/<name> volume (flushing metadata).  The mountpoint itself
 * persists as an empty mountpoint (Linux-style: umount leaves the directory).
 * NULL/empty unmounts the sole *bound* mount if exactly one exists.  Returns
 * 0, -1 (no such mount), -15 (not mounted), or -20 (ambiguous: name needed). */
int vfs_umount_hd(const char *name)
{
    if (!name || !*name) {
        int only = -1, nbound = 0;
        for (int i = 0; i < s_nmounts; i++)
            if (s_mounts[i].fs != HD_FS_NONE) { nbound++; only = i; }
        if (nbound == 1) name = s_mounts[only].name;
        else return -20;
    }
    int mi = hd_find_name(name);
    if (mi < 0) return -1;
    if (s_mounts[mi].fs == HD_FS_NONE) return -15;   /* nothing bound here */
    if (s_mounts[mi].fs == HD_FS_EXT2) ext2_unmount();
    else                               fat32_unmount();
    s_fixup_name = s_mounts[mi].name;
    apply_cwd_fixup(fixup_cwd_unmounted);
    s_mounts[mi].fs = HD_FS_NONE;        /* revert to empty mountpoint */
    return 0;
}

int vfs_hd_mounted(void) { return hd_mounted(); }

const char *vfs_hd_fsname(const char *name)
{
    int mi = name ? hd_find(name, strlen(name)) : -1;
    return (mi >= 0) ? hd_fsname(s_mounts[mi].fs) : "none";
}

void vfs_prepare_shutdown(void)
{
    /* Each backend's unmount flushes dirty metadata before clearing the
     * mount, so this is a clean sync on the way down. */
    for (int i = s_nmounts - 1; i >= 0; i--) {
        if (s_mounts[i].fs == HD_FS_NONE) continue;   /* empty mountpoint */
        t_writestring("Syncing /mnt/");
        t_writestring(s_mounts[i].name);
        t_writestring(" ...\n");
        if (s_mounts[i].fs == HD_FS_EXT2) ext2_unmount();
        else                              fat32_unmount();
    }
    s_nmounts = 0;
}

/*
 * vfs_notify_hd_mounted / _unmounted - compatibility shims for callers that
 * drive the FAT32 backend directly (auto-mount, installer): keep the canonical
 * "hd" FAT32 entry in the mount table in sync and run the cwd fixup.
 */
void vfs_notify_hd_mounted(void)
{
    int mi = hd_find("hd", 2);
    if (mi < 0 && s_nmounts < MAX_HD_MOUNTS) {
        mi = s_nmounts++;
        memcpy(s_mounts[mi].name, "hd", 3);
    }
    if (mi >= 0) s_mounts[mi].fs = HD_FS_FAT32;
    s_fixup_name = "hd";
    apply_cwd_fixup(fixup_cwd_mounted);
}

void vfs_notify_hd_unmounted(void)
{
    s_fixup_name = "hd";
    apply_cwd_fixup(fixup_cwd_unmounted);
    int mi = hd_find("hd", 2);
    if (mi >= 0) s_mounts[mi].fs = HD_FS_NONE;   /* revert to empty mountpoint */
}

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
 * try_mount_drive – probe 'drive' and mount its partition(s).
 *
 * Dual-partition layout (Makar installer):
 *   partition 0 – FAT32 boot  → /mnt/boot  (kernel + limine stage 3)
 *   partition 1 – ext2/FAT32  → /mnt/root  (apps / docs / src)
 *
 * Single-partition layout (test disk, legacy):
 *   partition 0 – FAT32       → /mnt/hd    (backward compatibility)
 *
 * Returns 1 if at least one partition was mounted, 0 otherwise.
 */
static int try_mount_drive(uint8_t drive)
{
    const ide_drive_t *d = ide_get_drive(drive);
    if (!d || !d->present || d->type != IDE_TYPE_ATA)
        return 0;

    if (part_probe(drive, &s_auto_parts) != 0)
        return 0;

    if (s_auto_parts.count == 0)
        return 0;

    /* ---- Single-partition path: legacy /mnt/hd ---- */
    if (s_auto_parts.count == 1) {
        const part_info_t *p = &s_auto_parts.parts[0];
        if (fat32_mount(drive, p->lba_start) == 0) {
            vfs_notify_hd_mounted();
            t_writestring("Auto-mounted FAT32 (drive ");
            t_dec(drive);
            t_writestring(", partition 1) at /mnt/hd\n");
            return 1;
        }
        return 0;
    }

    /* ---- Dual-partition path ---- */
    int mounted = 0;

    /* Partition 2 → /mnt/root (data: ext2 or FAT32). */
    const part_info_t *data_p = &s_auto_parts.parts[1];
    int data_mi = hd_find_name("root");
    if (data_mi >= 0 && s_mounts[data_mi].fs == HD_FS_NONE) {
        int fs = ext2_probe(drive, data_p->lba_start) ? HD_FS_EXT2 : HD_FS_FAT32;
        int r  = (fs == HD_FS_EXT2) ? ext2_mount(drive, data_p->lba_start)
                                     : fat32_mount(drive, data_p->lba_start);
        if (r == 0) {
            s_mounts[data_mi].fs = fs;
            s_fixup_name = s_mounts[data_mi].name;
            apply_cwd_fixup(fixup_cwd_mounted);
            t_writestring("Auto-mounted ");
            t_writestring(hd_fsname(fs));
            t_writestring(" (drive ");
            t_dec(drive);
            t_writestring(", partition 2) at /mnt/root\n");
            mounted = 1;
        }
    }

    /* Partition 1 → /mnt/boot (FAT32 boot, only if FAT32 backend is free). */
    const part_info_t *boot_p = &s_auto_parts.parts[0];
    int boot_is_fat32 = (s_auto_parts.scheme == PART_SCHEME_MBR)
        ? (boot_p->mbr_type == PART_MBR_FAT32_CHS ||
           boot_p->mbr_type == PART_MBR_FAT32_LBA)
        : (memcmp(boot_p->type_guid, PART_GUID_FAT32, 16) == 0);

    int boot_mi = hd_find_name("boot");
    if (boot_is_fat32 && boot_mi >= 0 && s_mounts[boot_mi].fs == HD_FS_NONE
            && !backend_in_use(HD_FS_FAT32)) {
        if (fat32_mount(drive, boot_p->lba_start) == 0) {
            s_mounts[boot_mi].fs = HD_FS_FAT32;
            t_writestring("Auto-mounted FAT32 (drive ");
            t_dec(drive);
            t_writestring(", partition 1) at /mnt/boot\n");
        }
    }

    return mounted;
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
            hd_mounted = try_mount_drive(hint_drive);
    }

    /* Exhaustive scan: try every slot in order. */
    for (int i = 0; i < IDE_MAX_DRIVES && !hd_mounted; i++)
        hd_mounted = try_mount_drive((uint8_t)i);

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
    int fs;
    switch (vfs_route(abs, &drv, &fs)) {
    case VFS_FS_ROOT:
        ls_root();
        return 0;

    case VFS_FS_MNT:
        ls_mnt();
        return 0;

    case VFS_FS_MNT_EMPTY:
        /* Empty mountpoint - an empty directory until something is mounted. */
        t_writestring("(empty mountpoint - mount a filesystem here)\n");
        return 0;

    case VFS_FS_HD:
        return hd_ls(fs, drv);

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
    int hdfs;
    int fs = vfs_route(abs, &drv, &hdfs);
    char *cwd = cwd_buf();

    switch (fs) {
    case VFS_FS_ROOT:
    case VFS_FS_MNT:
    case VFS_FS_MNT_EMPTY:
        /* Root, the /mnt container, and empty mountpoints are all valid
         * directories (an empty mountpoint is just an empty dir). */
        memcpy(cwd, abs, (size_t)(strlen(abs) + 1u));
        return 0;

    case VFS_FS_HD:
        /* Validate via the backend (FAT32 also updates its internal CWD). */
        if (hd_cd(hdfs, drv) != 0) {
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
    int hdfs;
    int fs = vfs_route(abs, &drv, &hdfs);

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
        err = hd_read_file(hdfs, drv, buf, CAT_MAX, &got);
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

/* If 'abs' is an immediate child of /mnt (i.e. "/mnt/<name>" with no deeper
 * component), copy <name> into out[] and return 1; else return 0. */
static int mnt_leaf(const char *abs, char *out, size_t outsz)
{
    if (memcmp(abs, VFS_MNT, VFS_MNT_LEN) != 0 || abs[VFS_MNT_LEN] != '/')
        return 0;
    const char *name = abs + VFS_MNT_LEN + 1;
    if (*name == '\0') return 0;
    size_t i = 0;
    for (const char *q = name; *q; q++) {
        if (*q == '/') return 0;            /* deeper than one level */
        if (i + 1 >= outsz) return 0;
        out[i++] = *q;
    }
    out[i] = '\0';
    return 1;
}

/* -------------------------------------------------------------------------
 * vfs_mkdir – create a directory.  An immediate child of /mnt becomes a new
 * empty mountpoint (a place to `mount` a filesystem); deeper paths create a
 * real directory on the bound disk filesystem.
 * ---------------------------------------------------------------------- */
int vfs_mkdir(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);

    /* mkdir /mnt/<name> -> create an empty mountpoint. */
    char mpname[VFS_MOUNT_NAME_MAX];
    if (mnt_leaf(abs, mpname, sizeof(mpname))) {
        int r = vfs_make_mountpoint(mpname);
        switch (r) {
        case 0:   return 0;
        case -6:  t_writestring("mkdir: already exists: ");  t_writestring(abs); t_putchar('\n'); break;
        case -12: t_writestring("mkdir: mountpoint table full (max ");
                  t_dec(MAX_HD_MOUNTS); t_writestring(")\n"); break;
        case -13: t_writestring("mkdir: 'cdrom' is reserved\n"); break;
        default:  t_writestring("mkdir: bad mountpoint name: "); t_writestring(abs); t_putchar('\n'); break;
        }
        return -1;
    }

    const char *drv;
    int hdfs;
    int r = vfs_route(abs, &drv, &hdfs);
    if (r == VFS_FS_CDROM) {
        t_writestring("mkdir: /mnt/cdrom is a read-only filesystem\n");
        return -1;
    }
    if (r != VFS_FS_HD) {
        t_writestring("mkdir: only supported under a /mnt disk volume\n");
        return -1;
    }
    return hd_mkdir(hdfs, drv);
}

/* -------------------------------------------------------------------------
 * vfs_read_file / vfs_write_file
 * ---------------------------------------------------------------------- */
int vfs_read_file(const char *path, void *buf, uint32_t bufsz, uint32_t *out_sz)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);

    const char *drv;
    int hdfs;
    switch (vfs_route(abs, &drv, &hdfs)) {
    case VFS_FS_HD:
        return hd_read_file(hdfs, drv, buf, bufsz, out_sz);

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
    int hdfs;
    if (vfs_route(abs, &drv, &hdfs) != VFS_FS_HD) return -1;
    return hd_write_file(hdfs, drv, buf, size);
}

int vfs_delete_file(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);

    const char *drv;
    int hdfs;
    if (vfs_route(abs, &drv, &hdfs) != VFS_FS_HD) return -1;
    return hd_delete_file(hdfs, drv);
}

int vfs_delete_dir(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);

    /* rmdir /mnt/<name> -> remove an empty mountpoint (busy if a fs is bound). */
    char mpname[VFS_MOUNT_NAME_MAX];
    if (mnt_leaf(abs, mpname, sizeof(mpname))) {
        int r = vfs_remove_mountpoint(mpname);
        if (r == -16) { t_writestring("rmdir: mountpoint busy - umount first\n"); return -1; }
        return r;   /* 0, or -1 no such mountpoint */
    }

    const char *drv;
    int hdfs;
    if (vfs_route(abs, &drv, &hdfs) != VFS_FS_HD) return -1;
    return hd_delete_dir(hdfs, drv);
}

int vfs_rename(const char *old_path, const char *new_path)
{
    char old_abs[VFS_PATH_MAX], new_abs[VFS_PATH_MAX];
    path_resolve(old_path, old_abs);
    path_resolve(new_path, new_abs);

    const char *old_drv, *new_drv;
    int ofs, nfs;
    if (vfs_route(old_abs, &old_drv, &ofs) != VFS_FS_HD) return -1;
    if (vfs_route(new_abs, &new_drv, &nfs) != VFS_FS_HD) return -1;
    if (ofs != nfs) return -1;   /* cross-filesystem rename unsupported */

    /* Check if source is a file or directory, then call appropriate rename. */
    if (ofs == HD_FS_EXT2)
        return hd_file_exists(ofs, old_drv) ? ext2_rename_file(old_drv, new_drv)
                                            : ext2_rename_dir(old_drv, new_drv);
    if (fat32_file_exists(old_drv))
        return fat32_rename_file(old_drv, new_drv);
    return fat32_rename_dir(old_drv, new_drv);
}

int vfs_file_exists(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);

    const char *drv;
    int hdfs;
    switch (vfs_route(abs, &drv, &hdfs)) {
    case VFS_FS_HD:
        return hd_file_exists(hdfs, drv);
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
    if (vfs_route(abs, &drv, NULL) != VFS_FS_DEV) return -1;
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
 * Returns 0 on success, -1 if the path is not under /mnt/hd or not mounted.
 * ---------------------------------------------------------------------- */
int vfs_complete(const char *dir, const char *prefix,
                 fat32_complete_cb_t cb, void *ctx)
{
    char abs[VFS_PATH_MAX];
    path_resolve(dir ? dir : cwd_buf(), abs);

    const char *drv;
    int hdfs;
    switch (vfs_route(abs, &drv, &hdfs)) {
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
            for (int i = 0; i < s_nmounts; i++) cb(s_mounts[i].name, 1, ctx);
            if (s_cdrom_drive >= 0) cb("cdrom", 1, ctx);
        }
        return 0;
    }
    case VFS_FS_MNT_EMPTY:
        return 0;   /* empty mountpoint - nothing to complete */
    case VFS_FS_HD:
        return hd_complete(hdfs, drv, prefix, cb, ctx);
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
