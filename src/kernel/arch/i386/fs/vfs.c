/*
 * vfs.c - Virtual Filesystem routing layer (mount-table model).
 *
 * Path namespace:
 *   /              rootfs (elected by vfs_mount_root: HD ext2/FAT32 if
 *                  installed, else CD-ROM ISO9660; explicit `root=/dev/hdaN`
 *                  cmdline arg overrides).
 *   /dev /proc     synthetic block-device + process trees (devfs, procfs).
 *   /tmp /log      synthetic in-RAM writable overlays (tmpfs, logfs).
 *   /boot          FAT32 boot partition when installed (limine + kernel).
 *   /mnt/<name>    user-mountable disk volumes (mkdir /mnt/<name> then
 *                  `mount /dev/hdaN /mnt/<name>`).
 *   /mnt/cdrom     CD-ROM ISO9660 (auto-registered when ATAPI present).
 *
 * Routing is longest-prefix-match against a single mount table
 * (`s_mounts[]`).  Each entry carries the mountpoint string, the
 * backend enum, and the per-backend handle (drive+lba for block
 * filesystems, slot name for /mnt/<name> bookkeeping).  The same
 * backing volume may legitimately appear under two mountpoints --
 * e.g. CD-ROM at `/mnt/cdrom` and `/` when it's the elected rootfs --
 * and both paths route to the same backend.
 *
 * All VFS paths are absolute after path_normalize.  Relative paths
 * are joined against the calling task's cwd (task_current()->cwd).
 * During boot (before tasking_init), there is no task_current();
 * writers fall back to s_boot_cwd, which tasking_init hands off to
 * idle->cwd via vfs_getcwd().  Post-tasking, every cwd read/write is
 * per-task, so VT0 may sit in /apps while VT1 sits in /proc without
 * cross-contamination.
 */

#include <kernel/vfs.h>
#include <kernel/fat32.h>
#include <kernel/ext2.h>
#include <kernel/iso9660.h>
#include <kernel/procfs.h>
#include <kernel/devfs.h>
#include <kernel/logfs.h>
#include <kernel/tmpfs.h>
#include <kernel/ide.h>
#include <kernel/partition.h>
#include <kernel/tty.h>
#include <kernel/heap.h>
#include <kernel/task.h>
#include <string.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Mount table
 * ---------------------------------------------------------------------- */

#define VFS_MOUNT_NAME_MAX 32

/* Per-shell rootfs sentinel: presence of this file marks a volume as a
 * candidate for rootfs election (matches the historical resolve_rootfs
 * probe). */
#define ROOTFS_SENTINEL "/usr/lib/crt0.o"

typedef enum {
    VFS_BACKEND_NONE    = 0,   /* empty mountpoint placeholder           */
    VFS_BACKEND_EXT2    = 1,
    VFS_BACKEND_FAT32   = 2,
    VFS_BACKEND_ISO9660 = 3,
    VFS_BACKEND_DEVFS   = 4,
    VFS_BACKEND_PROCFS  = 5,
    VFS_BACKEND_TMPFS   = 6,
    VFS_BACKEND_LOGFS   = 7,
} vfs_backend_t;

typedef struct {
    char           mountpoint[VFS_PATH_MAX];
    vfs_backend_t  backend;
    uint8_t        drive;                       /* ext2/fat32/iso9660 */
    uint32_t       lba;                         /* ext2/fat32         */
    /* Bookkeeping for /mnt/<name> entries.  Non-empty for any
     * entry whose mountpoint starts with "/mnt/" -- carries the bare
     * slot name so `vfs_hd_fsname("root")` etc. can still look up by
     * legacy name. */
    char           slot_name[VFS_MOUNT_NAME_MAX];
} vfs_mount_t;

#define MAX_MOUNTS 16
static vfs_mount_t s_mounts[MAX_MOUNTS];
static int         s_nmounts;

/* CD-ROM IDE drive index (one ATAPI volume max), -1 if none.  Recorded
 * by vfs_init's probe; consumed by vfs_mount_root's emergency fallback
 * and by the auto-mount of /mnt/cdrom. */
static int      s_cdrom_drive   = -1;
static uint32_t s_boot_biosdev  = 0xFFu;

/* Pre-tasking-init scratch cwd; handed off to idle->cwd by tasking_init. */
static char s_boot_cwd[VFS_PATH_MAX] = "/";

/* Resolve the cwd backing store for the calling context.  Pre-tasking
 * (vfs_init, vfs_mount_root, vfs_auto_mount) returns the boot scratch
 * buffer; once tasking is up, every reader/writer hits the calling
 * task's own cwd field. */
static char *cwd_buf(void)
{
    task_t *t = task_current();
    return t ? t->cwd : s_boot_cwd;
}

/* -------------------------------------------------------------------------
 * /log compatibility shims (kernel-facing klog API targets /log/kernel.log)
 * ---------------------------------------------------------------------- */
void vfs_klog_reset(void)                      { logfs_kreset(); }
void vfs_klog_write(const char *s, uint32_t n) { logfs_kwrite(s, n); }
void vfs_klog_append(const char *line)         { logfs_append_line("kernel.log", line); }

/* -------------------------------------------------------------------------
 * Mount table primitives
 * ---------------------------------------------------------------------- */

/* Find a mount entry by exact mountpoint string.  Returns index or -1. */
static int mount_find_exact(const char *mountpoint)
{
    for (int i = 0; i < s_nmounts; i++)
        if (strcmp(s_mounts[i].mountpoint, mountpoint) == 0) return i;
    return -1;
}

/* Find a /mnt/<name> entry by bare slot name.  Returns index or -1. */
static int mount_find_slot(const char *name)
{
    if (!name || !*name) return -1;
    for (int i = 0; i < s_nmounts; i++)
        if (s_mounts[i].slot_name[0] && strcmp(s_mounts[i].slot_name, name) == 0)
            return i;
    return -1;
}

/* Append a new mount entry.  Returns the new entry's index, or -1 if
 * the table is full. */
static int mount_add(const char *mountpoint,
                     vfs_backend_t backend,
                     uint8_t drive, uint32_t lba,
                     const char *slot_name)
{
    if (s_nmounts >= MAX_MOUNTS) return -1;
    vfs_mount_t *m = &s_mounts[s_nmounts];
    size_t mn = strlen(mountpoint);
    if (mn >= VFS_PATH_MAX) return -1;
    memcpy(m->mountpoint, mountpoint, mn + 1);
    m->backend = backend;
    m->drive   = drive;
    m->lba     = lba;
    m->slot_name[0] = '\0';
    if (slot_name && *slot_name) {
        size_t sn = strlen(slot_name);
        if (sn >= VFS_MOUNT_NAME_MAX) sn = VFS_MOUNT_NAME_MAX - 1;
        memcpy(m->slot_name, slot_name, sn);
        m->slot_name[sn] = '\0';
    }
    s_nmounts++;
    return s_nmounts - 1;
}

/* Remove the entry at `idx`.  Compacts the table. */
static void mount_remove(int idx)
{
    if (idx < 0 || idx >= s_nmounts) return;
    for (int i = idx; i + 1 < s_nmounts; i++) s_mounts[i] = s_mounts[i + 1];
    s_nmounts--;
}

/* True if any entry's backend equals `b`. */
static int backend_in_use(vfs_backend_t b)
{
    for (int i = 0; i < s_nmounts; i++) if (s_mounts[i].backend == b) return 1;
    return 0;
}

/* True if a mount entry already points at exactly this block volume.  The
 * ext2/fat32 drivers keep one global mounted volume each, so a second VFS
 * entry for the same (backend, drive, lba) is a bind alias, not a remount. */
static int backend_volume_in_use(vfs_backend_t b, uint8_t drive, uint32_t lba)
{
    for (int i = 0; i < s_nmounts; i++) {
        if (s_mounts[i].backend == b &&
            s_mounts[i].drive == drive &&
            s_mounts[i].lba == lba)
            return 1;
    }
    return 0;
}

/* True if some entry other than idx still references the same mounted
 * backend volume.  Used so unmounting a bind alias does not tear down the
 * elected rootfs backend out from under "/". */
static int mount_has_alias(int idx)
{
    if (idx < 0 || idx >= s_nmounts) return 0;
    vfs_mount_t *m = &s_mounts[idx];
    for (int i = 0; i < s_nmounts; i++) {
        if (i == idx) continue;
        if (s_mounts[i].backend == m->backend &&
            s_mounts[i].drive == m->drive &&
            s_mounts[i].lba == m->lba)
            return 1;
    }
    return 0;
}

/* True if the entry at `idx` is mounted (backend != NONE). */
static int mount_is_bound(int idx)
{
    return idx >= 0 && idx < s_nmounts && s_mounts[idx].backend != VFS_BACKEND_NONE;
}

/* Backend name (used by `ls /mnt` and the legacy vfs_hd_fsname API). */
static const char *backend_name(vfs_backend_t b)
{
    switch (b) {
    case VFS_BACKEND_EXT2:    return "ext2";
    case VFS_BACKEND_FAT32:   return "FAT32";
    case VFS_BACKEND_ISO9660: return "ISO9660";
    case VFS_BACKEND_DEVFS:   return "devfs";
    case VFS_BACKEND_PROCFS:  return "procfs";
    case VFS_BACKEND_TMPFS:   return "tmpfs";
    case VFS_BACKEND_LOGFS:   return "logfs";
    default:                  return "none";
    }
}

/* -------------------------------------------------------------------------
 * path_normalize / path_resolve
 * ---------------------------------------------------------------------- */

static void path_normalize(const char *in, char *out, int outsz)
{
    int comp_start[VFS_PATH_MAX / 2];
    int top  = 0;
    int olen = 0;
    if (outsz > 1) out[olen++] = '/';
    const char *p = in;
    if (*p == '/') p++;
    while (*p) {
        const char *seg = p;
        while (*p && *p != '/') p++;
        int slen = (int)(p - seg);
        if (*p == '/') p++;
        if (slen == 0 || (slen == 1 && seg[0] == '.')) continue;
        if (slen == 2 && seg[0] == '.' && seg[1] == '.') {
            if (top > 0) olen = comp_start[--top];
            continue;
        }
        if (top < VFS_PATH_MAX / 2) comp_start[top++] = olen;
        if (olen > 1 && olen < outsz - 1) out[olen++] = '/';
        for (int i = 0; i < slen && olen < outsz - 1; i++)
            out[olen++] = seg[i];
    }
    out[olen] = '\0';
}

static void path_resolve(const char *path, char *out)
{
    char tmp[VFS_PATH_MAX * 2];
    const char *cwd = cwd_buf();
    if (!path || !*path) { path_normalize(cwd, out, VFS_PATH_MAX); return; }
    if (path[0] == '/')   { path_normalize(path, out, VFS_PATH_MAX); return; }
    int clen = (int)strlen(cwd);
    int plen = (int)strlen(path);
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
 * vfs_route -- longest-prefix-match against the mount table.
 *
 * Returns the matched mount entry's index (>= 0), or -1 if no mount
 * covers `abs` (the no-rootfs / empty `/mnt` virtual-dir case).
 *
 * `*drv_path` is set to the driver-relative path within the matched
 * mount: e.g. abs = "/dev/hda1", matched mountpoint = "/dev", drv_path
 * = "/hda1".  For root-level matches (mountpoint = "/"), drv_path is
 * just `abs`.  For exact-prefix matches (abs == mountpoint), drv_path
 * is "/" so backends always see a leading slash.
 * ---------------------------------------------------------------------- */
static int vfs_route(const char *abs, const char **drv_path)
{
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < s_nmounts; i++) {
        const char *mp = s_mounts[i].mountpoint;
        size_t      mn = strlen(mp);
        if (mn == 1 && mp[0] == '/') {
            /* "/" matches everything; keep as fallback unless a longer
             * prefix is found. */
            if (best < 0) { best = i; best_len = 1; }
            continue;
        }
        if (strncmp(abs, mp, mn) != 0) continue;
        char nxt = abs[mn];
        if (nxt != '\0' && nxt != '/') continue;   /* must end on boundary */
        if (mn > best_len) { best = i; best_len = mn; }
    }
    if (best < 0) { *drv_path = abs; return -1; }
    const char *mp = s_mounts[best].mountpoint;
    size_t      mn = strlen(mp);
    if (mn == 1 && mp[0] == '/') {
        *drv_path = abs;   /* root mount: pass abs as-is */
    } else {
        const char *rest = abs + mn;
        *drv_path = (*rest == '/') ? rest : "/";
    }
    return best;
}

/* -------------------------------------------------------------------------
 * Backend dispatch helpers (one per VFS operation; switch on backend).
 *
 * Each accepts a vfs_mount_t* (so iso9660 ops have access to drive)
 * and the driver-relative path.  These centralise the per-backend
 * call so the higher-level vfs_* functions stay legible.
 * ---------------------------------------------------------------------- */

static int backend_ls(vfs_mount_t *m, const char *p)
{
    switch (m->backend) {
    case VFS_BACKEND_EXT2:    return ext2_ls(p);
    case VFS_BACKEND_FAT32:   return fat32_ls(p);
    case VFS_BACKEND_ISO9660: return iso9660_ls(m->drive, p);
    case VFS_BACKEND_DEVFS:   return devfs_ls(p);
    case VFS_BACKEND_PROCFS:  return procfs_ls(p);
    case VFS_BACKEND_TMPFS:   return tmpfs_ls(p);
    case VFS_BACKEND_LOGFS:   return logfs_ls(p);
    default:                  return -1;
    }
}

static int backend_cd(vfs_mount_t *m, const char *p)
{
    switch (m->backend) {
    case VFS_BACKEND_EXT2:  return ext2_cd(p);
    case VFS_BACKEND_FAT32: return fat32_cd(p);
    default:                return 0;   /* flat backends accept any "/X" */
    }
}

static int backend_read_file(vfs_mount_t *m, const char *p,
                              void *buf, uint32_t bufsz, uint32_t *out_sz)
{
    switch (m->backend) {
    case VFS_BACKEND_EXT2:    return ext2_read_file(p, buf, bufsz, out_sz);
    case VFS_BACKEND_FAT32:   return fat32_read_file(p, buf, bufsz, out_sz);
    case VFS_BACKEND_ISO9660: return iso9660_read_file(m->drive, p, buf, bufsz, out_sz);
    case VFS_BACKEND_PROCFS:  return procfs_read_file(p, buf, bufsz, out_sz);
    case VFS_BACKEND_LOGFS:   return (logfs_read(p, buf, bufsz, out_sz) < 0) ? -1 : 0;
    case VFS_BACKEND_TMPFS:   return (tmpfs_read(p, buf, bufsz, out_sz) < 0) ? -1 : 0;
    case VFS_BACKEND_DEVFS: {
        int idx = devfs_lookup(p);
        if (idx < 0) return -1;
        long r = devfs_pread(idx, buf, bufsz, 0);
        if (r < 0) return -1;
        if (out_sz) *out_sz = (uint32_t)r;
        return 0;
    }
    default: return -1;
    }
}

static int backend_write_file(vfs_mount_t *m, const char *p,
                               const void *buf, uint32_t size)
{
    switch (m->backend) {
    case VFS_BACKEND_EXT2:  return ext2_write_file(p, buf, size);
    case VFS_BACKEND_FAT32: return fat32_write_file(p, buf, size);
    case VFS_BACKEND_LOGFS:
        /* /log is kernel-write only (Linux's /var/log model).  klog_write
         * + friends in logfs.c reach the ring directly via ring_append;
         * userspace and the shell route through vfs_write_file here, and
         * land on this rejection -- matches the ISO9660 mkdir reject in
         * vfs_mkdir.  Use /tmp for user-writable scratch files. */
        (void)p; (void)buf; (void)size;
        t_writestring("write: read-only filesystem (/log)\n");
        return -1;
    case VFS_BACKEND_TMPFS: return (tmpfs_write(p, buf, size) < 0) ? -1 : 0;
    default: return -1;   /* read-only or non-writable backend */
    }
}

static int backend_mkdir(vfs_mount_t *m, const char *p)
{
    switch (m->backend) {
    case VFS_BACKEND_EXT2:  return ext2_mkdir(p);
    case VFS_BACKEND_FAT32: return fat32_mkdir(p);
    default: return -1;
    }
}

static int backend_delete_file(vfs_mount_t *m, const char *p)
{
    switch (m->backend) {
    case VFS_BACKEND_EXT2:  return ext2_delete_file(p);
    case VFS_BACKEND_FAT32: return fat32_delete_file(p);
    case VFS_BACKEND_TMPFS: return tmpfs_delete(p);
    default: return -1;
    }
}

static int backend_delete_dir(vfs_mount_t *m, const char *p)
{
    switch (m->backend) {
    case VFS_BACKEND_EXT2:  return ext2_delete_dir(p);
    case VFS_BACKEND_FAT32: return fat32_delete_dir(p);
    default: return -1;
    }
}

static int backend_file_exists(vfs_mount_t *m, const char *p)
{
    switch (m->backend) {
    case VFS_BACKEND_EXT2:    return ext2_file_exists(p);
    case VFS_BACKEND_FAT32:   return fat32_file_exists(p);
    case VFS_BACKEND_ISO9660: return iso9660_file_exists(m->drive, p);
    case VFS_BACKEND_PROCFS:  return procfs_file_exists(p);
    case VFS_BACKEND_DEVFS:   return devfs_file_exists(p);
    case VFS_BACKEND_LOGFS:   return logfs_file_exists(p);
    case VFS_BACKEND_TMPFS:   return tmpfs_file_exists(p);
    default: return 0;
    }
}

static int backend_complete(vfs_mount_t *m, const char *p, const char *pre,
                             fat32_complete_cb_t cb, void *ctx)
{
    switch (m->backend) {
    case VFS_BACKEND_EXT2:    return ext2_complete(p, pre, cb, ctx);
    case VFS_BACKEND_FAT32:   return fat32_complete(p, pre, cb, ctx);
    case VFS_BACKEND_ISO9660: return iso9660_complete(m->drive, p, pre, cb, ctx);
    case VFS_BACKEND_PROCFS:  return procfs_complete(p, pre, cb, ctx);
    case VFS_BACKEND_DEVFS:   return devfs_complete(p, pre, cb, ctx);
    case VFS_BACKEND_LOGFS:   return logfs_complete(p, pre, cb, ctx);
    case VFS_BACKEND_TMPFS:   return tmpfs_complete(p, pre, cb, ctx);
    default: return -1;
    }
}

static void backend_unmount(vfs_backend_t b)
{
    switch (b) {
    case VFS_BACKEND_EXT2:  ext2_unmount(); break;
    case VFS_BACKEND_FAT32: fat32_unmount(); break;
    default: break;   /* in-RAM + read-only backends have nothing to flush */
    }
}

/* -------------------------------------------------------------------------
 * cwd fixups (mount transitions can leave tasks stranded)
 * ---------------------------------------------------------------------- */

typedef void (*cwd_fixup_fn)(char *cwd, const char *path);

static void fixup_cwd_mounted(char *cwd, const char *path)
{
    /* Just-mounted a volume at `path`: pull cwd from "/" to `path` so
     * the user immediately sees the freshly-bound mount.  Matches the
     * pre-refactor behaviour (boot scratch cwd seeded to the new mount). */
    if (strcmp(cwd, "/") == 0) {
        size_t pn = strlen(path);
        if (pn >= VFS_PATH_MAX) pn = VFS_PATH_MAX - 1;
        memcpy(cwd, path, pn);
        cwd[pn] = '\0';
    }
}

static void fixup_cwd_unmounted(char *cwd, const char *path)
{
    /* Just-unmounted at `path`: yank tasks parked under it back to "/". */
    size_t pn = strlen(path);
    if (strcmp(cwd, path) == 0 ||
        (strncmp(cwd, path, pn) == 0 && cwd[pn] == '/')) {
        cwd[0] = '/';
        cwd[1] = '\0';
    }
}

static void apply_cwd_fixup(cwd_fixup_fn fn, const char *path)
{
    fn(s_boot_cwd, path);
    for (int i = 0; ; i++) {
        task_t *t = task_get(i);
        if (!t) break;
        fn(t->cwd, path);
    }
}

/* =========================================================================
 * Public API
 * ====================================================================== */

void vfs_init(void)
{
    s_boot_cwd[0] = '/';
    s_boot_cwd[1] = '\0';
    s_cdrom_drive = -1;
    s_nmounts     = 0;

    /* Probe IDE bus for an ATAPI CD-ROM with a valid ISO9660 volume. */
    for (int i = 0; i < IDE_MAX_DRIVES; i++) {
        const ide_drive_t *d = ide_get_drive((uint8_t)i);
        if (!d || !d->present || d->type != IDE_TYPE_ATAPI) continue;
        if (iso9660_probe((uint8_t)i) == 0) {
            s_cdrom_drive = i;
            break;
        }
    }

    /* Register synthetic overlays.  These are always present and don't
     * depend on the rootfs being elected. */
    mount_add("/dev",  VFS_BACKEND_DEVFS,  0, 0, "");
    mount_add("/proc", VFS_BACKEND_PROCFS, 0, 0, "");
    mount_add("/tmp",  VFS_BACKEND_TMPFS,  0, 0, "");
    mount_add("/log",  VFS_BACKEND_LOGFS,  0, 0, "");

    /* CD-ROM at /mnt/cdrom if present (slot_name "cdrom" so the legacy
     * `umount /mnt/cdrom` and similar still resolve). */
    if (s_cdrom_drive >= 0)
        mount_add("/mnt/cdrom", VFS_BACKEND_ISO9660,
                  (uint8_t)s_cdrom_drive, 0, "cdrom");

    /* Pre-register the boot/root slot placeholders (empty mountpoints
     * until vfs_auto_mount binds backends to them).  These are kept as
     * placeholders even on CD-only boots so the installer can mount
     * /mnt/root without a pre-`mkdir`. */
    mount_add("/mnt/boot", VFS_BACKEND_NONE, 0, 0, "boot");
    mount_add("/mnt/root", VFS_BACKEND_NONE, 0, 0, "root");

    /* Build the /dev node table from the just-scanned IDE bus. */
    devfs_init();
}

const char *vfs_getcwd(void) { return cwd_buf(); }

void vfs_set_boot_drive(uint32_t biosdev) { s_boot_biosdev = biosdev; }

/* -------------------------------------------------------------------------
 * Rootfs election (A2): vfs_mount_root
 *
 * Called by kernel_main between vfs_init and vfs_auto_mount.
 *
 *   spec      -- value of the Multiboot2 `root=` cmdline arg, or NULL.
 *                Forms: "/dev/hdaN", "auto" (= default), or "none".
 *
 * Election order:
 *   1. If spec is a /dev node, resolve via devfs_lookup and try
 *      ext2 then FAT32; on backend-mount success, register at "/".
 *   2. If spec is "none", skip election (developer / no-rootfs boot).
 *   3. Auto-detect: walk every ATA drive's partitions, ext2 → FAT32
 *      probe each; the first volume that holds ROOTFS_SENTINEL wins.
 *   4. Fallback: if the CD-ROM was probed in vfs_init AND it holds
 *      ROOTFS_SENTINEL, elect the CD as rootfs.
 *   5. Nothing matched: log it and continue; "/" stays unrouted, and
 *      `ls /` will only show the synthetic overlays.
 * ---------------------------------------------------------------------- */

static disk_parts_t s_auto_parts;   /* shared scratch for partition probes */

/* True if `drive` lba `lba` (ext2/fat32) contains ROOTFS_SENTINEL.
 * We mount the volume, probe, then unmount; matches the historical
 * resolve_rootfs sentinel probe. */
static int volume_has_sentinel(vfs_backend_t b, uint8_t drive, uint32_t lba)
{
    /* Already-bound backends can be reused; otherwise temporarily mount. */
    int already = backend_in_use(b);
    if (!already) {
        int r = (b == VFS_BACKEND_EXT2) ? ext2_mount(drive, lba)
                                         : fat32_mount(drive, lba);
        if (r != 0) return 0;
    }
    int ok = (b == VFS_BACKEND_EXT2) ? ext2_file_exists(ROOTFS_SENTINEL)
                                      : fat32_file_exists(ROOTFS_SENTINEL);
    if (!already) {
        if (b == VFS_BACKEND_EXT2) ext2_unmount();
        else                       fat32_unmount();
    }
    return ok;
}

/* Try to mount (drive, lba) at "/" with the given backend.  Returns 0
 * on success.  Skipped if that backend is already in use elsewhere. */
static int try_mount_root(vfs_backend_t b, uint8_t drive, uint32_t lba)
{
    if (backend_in_use(b)) return -1;
    int r = (b == VFS_BACKEND_EXT2) ? ext2_mount(drive, lba)
                                     : fat32_mount(drive, lba);
    if (r != 0) return r;
    if (mount_add("/", b, drive, lba, "") < 0) {
        backend_unmount(b);
        return -1;
    }
    t_writestring("Rootfs: ");
    t_writestring(backend_name(b));
    t_writestring(" (drive ");
    t_dec(drive);
    t_writestring(") at /\n");
    return 0;
}

void vfs_mount_root(const char *spec)
{
    /* 1. Explicit "none" -- skip election. */
    if (spec && strcmp(spec, "none") == 0) {
        t_writestring("Rootfs: skipped (root=none)\n");
        return;
    }

    /* 2. Explicit /dev/hdaN spec. */
    if (spec && spec[0] == '/' && strncmp(spec, "/dev/", 5) == 0) {
        uint8_t  drive;
        uint32_t lba;
        int      node = devfs_lookup(spec + 4);   /* "/dev/hda1" -> "/hda1" */
        if (node >= 0 && devfs_node_location(node, &drive, &lba) == 0) {
            if (ext2_probe(drive, lba)) {
                if (try_mount_root(VFS_BACKEND_EXT2, drive, lba) == 0) return;
            }
            if (try_mount_root(VFS_BACKEND_FAT32, drive, lba) == 0) return;
            t_writestring("Rootfs: root=");
            t_writestring(spec);
            t_writestring(" -- mount failed, falling back to auto-detect\n");
        }
    }

    /* 3. Auto-detect: walk every ATA drive's partitions. */
    for (int d = 0; d < IDE_MAX_DRIVES; d++) {
        const ide_drive_t *dr = ide_get_drive((uint8_t)d);
        if (!dr || !dr->present || dr->type != IDE_TYPE_ATA) continue;
        if (part_probe((uint8_t)d, &s_auto_parts) != 0) continue;
        for (int p = 0; p < s_auto_parts.count; p++) {
            uint32_t lba = s_auto_parts.parts[p].lba_start;
            if (ext2_probe((uint8_t)d, lba) &&
                volume_has_sentinel(VFS_BACKEND_EXT2, (uint8_t)d, lba)) {
                if (try_mount_root(VFS_BACKEND_EXT2, (uint8_t)d, lba) == 0) return;
            }
            if (volume_has_sentinel(VFS_BACKEND_FAT32, (uint8_t)d, lba)) {
                if (try_mount_root(VFS_BACKEND_FAT32, (uint8_t)d, lba) == 0) return;
            }
        }
    }

    /* 4. CD-ROM fallback (live boot). */
    if (s_cdrom_drive >= 0 &&
        iso9660_file_exists((uint8_t)s_cdrom_drive, ROOTFS_SENTINEL)) {
        if (mount_add("/", VFS_BACKEND_ISO9660,
                      (uint8_t)s_cdrom_drive, 0, "") >= 0) {
            t_writestring("Rootfs: ISO9660 CD-ROM at / (live boot)\n");
            return;
        }
    }

    /* 5. Nothing matched. */
    t_writestring("Rootfs: none found (/ is empty; overlays only)\n");
}

/* -------------------------------------------------------------------------
 * vfs_auto_mount: probe ATA drives and bind to /mnt/boot + /mnt/root.
 *
 * Single-partition disks bind at /mnt/root.  Dual-partition installer
 * layouts bind FAT32 boot at /mnt/boot and ext2/FAT32 data at /mnt/root.
 * After binding, if /mnt/boot got a FAT32 backend, also register a
 * second entry at "/boot" pointing at the same volume (so the user can
 * reach the boot partition without prefixing /mnt).
 * ---------------------------------------------------------------------- */

static int try_mount_drive(uint8_t drive)
{
    const ide_drive_t *d = ide_get_drive(drive);
    if (!d || !d->present || d->type != IDE_TYPE_ATA) return 0;
    if (part_probe(drive, &s_auto_parts) != 0) return 0;
    if (s_auto_parts.count == 0) return 0;

    int root_mi = mount_find_slot("root");

    /* Single-partition: bind at /mnt/root (rootfs election already
     * picked a winner if applicable). */
    if (s_auto_parts.count == 1) {
        if (root_mi < 0 || mount_is_bound(root_mi)) return 0;
        const part_info_t *p = &s_auto_parts.parts[0];
        vfs_backend_t b = ext2_probe(drive, p->lba_start) ? VFS_BACKEND_EXT2
                                                          : VFS_BACKEND_FAT32;
        if (backend_in_use(b)) {
            if (!backend_volume_in_use(b, drive, p->lba_start)) return 0;
        } else {
            int r = (b == VFS_BACKEND_EXT2) ? ext2_mount(drive, p->lba_start)
                                             : fat32_mount(drive, p->lba_start);
            if (r != 0) return 0;
        }
        s_mounts[root_mi].backend = b;
        s_mounts[root_mi].drive   = drive;
        s_mounts[root_mi].lba     = p->lba_start;
        apply_cwd_fixup(fixup_cwd_mounted, s_mounts[root_mi].mountpoint);
        t_writestring("Auto-mounted ");
        t_writestring(backend_name(b));
        t_writestring(" (drive ");
        t_dec(drive);
        t_writestring(", partition 1) at /mnt/root\n");
        return 1;
    }

    /* Dual-partition: partition 1 → /mnt/root (data). */
    int mounted = 0;
    const part_info_t *data_p = &s_auto_parts.parts[1];
    if (root_mi >= 0 && !mount_is_bound(root_mi)) {
        vfs_backend_t b = ext2_probe(drive, data_p->lba_start)
                            ? VFS_BACKEND_EXT2 : VFS_BACKEND_FAT32;
        if (backend_in_use(b) &&
            backend_volume_in_use(b, drive, data_p->lba_start)) {
            s_mounts[root_mi].backend = b;
            s_mounts[root_mi].drive   = drive;
            s_mounts[root_mi].lba     = data_p->lba_start;
            apply_cwd_fixup(fixup_cwd_mounted, s_mounts[root_mi].mountpoint);
            t_writestring("Auto-mounted ");
            t_writestring(backend_name(b));
            t_writestring(" (drive ");
            t_dec(drive);
            t_writestring(", partition 2) at /mnt/root\n");
            mounted = 1;
        } else if (!backend_in_use(b)) {
            int r = (b == VFS_BACKEND_EXT2)
                        ? ext2_mount(drive, data_p->lba_start)
                        : fat32_mount(drive, data_p->lba_start);
            if (r == 0) {
                s_mounts[root_mi].backend = b;
                s_mounts[root_mi].drive   = drive;
                s_mounts[root_mi].lba     = data_p->lba_start;
                apply_cwd_fixup(fixup_cwd_mounted, s_mounts[root_mi].mountpoint);
                t_writestring("Auto-mounted ");
                t_writestring(backend_name(b));
                t_writestring(" (drive ");
                t_dec(drive);
                t_writestring(", partition 2) at /mnt/root\n");
                mounted = 1;
            }
        }
    }

    /* Partition 0 → /mnt/boot (FAT32 only, if backend free). */
    const part_info_t *boot_p = &s_auto_parts.parts[0];
    int boot_is_fat32 = (s_auto_parts.scheme == PART_SCHEME_MBR)
        ? (boot_p->mbr_type == PART_MBR_FAT32_CHS ||
           boot_p->mbr_type == PART_MBR_FAT32_LBA)
        : (memcmp(boot_p->type_guid, PART_GUID_FAT32, 16) == 0);
    int boot_mi = mount_find_slot("boot");
    if (boot_is_fat32 && boot_mi >= 0 && !mount_is_bound(boot_mi)
            && !backend_in_use(VFS_BACKEND_FAT32)) {
        if (fat32_mount(drive, boot_p->lba_start) == 0) {
            s_mounts[boot_mi].backend = VFS_BACKEND_FAT32;
            s_mounts[boot_mi].drive   = drive;
            s_mounts[boot_mi].lba     = boot_p->lba_start;
            /* Also expose at /boot so `cat /boot/makar.kernel` works
             * without /mnt prefix (replaces the old s_bootfs scratch
             * rewrite). */
            mount_add("/boot", VFS_BACKEND_FAT32, drive, boot_p->lba_start, "");
            t_writestring("Auto-mounted FAT32 (drive ");
            t_dec(drive);
            t_writestring(", partition 1) at /mnt/boot (+ /boot)\n");
        }
    }
    return mounted;
}

void vfs_auto_mount(void)
{
    int hd_mounted = 0;
    if (s_boot_biosdev >= 0x80u && s_boot_biosdev <= 0xDFu) {
        uint8_t hint = (uint8_t)(s_boot_biosdev - 0x80u);
        if (hint < IDE_MAX_DRIVES) hd_mounted = try_mount_drive(hint);
    }
    for (int i = 0; i < IDE_MAX_DRIVES && !hd_mounted; i++)
        hd_mounted = try_mount_drive((uint8_t)i);
    if (s_cdrom_drive >= 0)
        t_writestring("CD-ROM detected, accessible at /mnt/cdrom\n");
}

/* -------------------------------------------------------------------------
 * vfs_ensure_root_home (A4): mkdir /root once on writable rootfs boots.
 * ---------------------------------------------------------------------- */
void vfs_ensure_root_home(void)
{
    int root_idx = mount_find_exact("/");
    if (root_idx < 0) return;
    vfs_backend_t b = s_mounts[root_idx].backend;
    if (b != VFS_BACKEND_EXT2 && b != VFS_BACKEND_FAT32) return;
    if (vfs_file_exists("/root")) return;
    (void)vfs_mkdir("/root");   /* best-effort */
}

/* -------------------------------------------------------------------------
 * Mount/umount/mkdir-mountpoint user API (unchanged semantics)
 * ---------------------------------------------------------------------- */

int vfs_mount_hd(uint8_t drive, uint32_t lba, const char *name, int *out_fs)
{
    if (!name || !*name) return -1;
    for (const char *q = name; *q; q++) if (*q == '/') return -1;
    if (strlen(name) >= VFS_MOUNT_NAME_MAX) return -1;
    if (strcmp(name, "cdrom") == 0) return -13;

    int mi = mount_find_slot(name);
    if (mi < 0)                  return -14;
    if (mount_is_bound(mi))      return -10;

    vfs_backend_t b = ext2_probe(drive, lba) ? VFS_BACKEND_EXT2
                                              : VFS_BACKEND_FAT32;
    if (backend_in_use(b))       return -11;
    int r = (b == VFS_BACKEND_EXT2) ? ext2_mount(drive, lba)
                                     : fat32_mount(drive, lba);
    if (r != 0) return r;
    s_mounts[mi].backend = b;
    s_mounts[mi].drive   = drive;
    s_mounts[mi].lba     = lba;
    apply_cwd_fixup(fixup_cwd_mounted, s_mounts[mi].mountpoint);
    if (out_fs) *out_fs = (b == VFS_BACKEND_EXT2) ? 2 : 1;   /* legacy enum */
    return 0;
}

int vfs_make_mountpoint(const char *name)
{
    if (!name || !*name) return -1;
    for (const char *q = name; *q; q++) if (*q == '/') return -1;
    if (strlen(name) >= VFS_MOUNT_NAME_MAX) return -1;
    if (strcmp(name, "cdrom") == 0) return -13;
    if (mount_find_slot(name) >= 0) return -6;
    char mp[VFS_PATH_MAX];
    if (5 + strlen(name) + 1 > VFS_PATH_MAX) return -1;
    strcpy(mp, "/mnt/");
    strcat(mp, name);
    if (mount_add(mp, VFS_BACKEND_NONE, 0, 0, name) < 0) return -12;
    return 0;
}

int vfs_remove_mountpoint(const char *name)
{
    int mi = mount_find_slot(name);
    if (mi < 0) return -1;
    if (mount_is_bound(mi)) return -16;
    mount_remove(mi);
    return 0;
}

int vfs_umount_hd(const char *name)
{
    /* NULL/empty: unmount the sole bound HD volume if unique. */
    if (!name || !*name) {
        int only = -1, nbound = 0;
        for (int i = 0; i < s_nmounts; i++) {
            if (!s_mounts[i].slot_name[0]) continue;   /* not a /mnt slot */
            vfs_backend_t b = s_mounts[i].backend;
            if (b == VFS_BACKEND_EXT2 || b == VFS_BACKEND_FAT32) {
                nbound++; only = i;
            }
        }
        if (nbound == 1) name = s_mounts[only].slot_name;
        else return -20;
    }
    int mi = mount_find_slot(name);
    if (mi < 0) return -1;
    if (!mount_is_bound(mi)) return -15;
    if (!mount_has_alias(mi))
        backend_unmount(s_mounts[mi].backend);
    apply_cwd_fixup(fixup_cwd_unmounted, s_mounts[mi].mountpoint);
    s_mounts[mi].backend = VFS_BACKEND_NONE;
    /* If a /boot promotion-mirror points at this volume, drop it. */
    int boot_idx = mount_find_exact("/boot");
    if (boot_idx >= 0 && strcmp(s_mounts[mi].slot_name, "boot") == 0)
        mount_remove(boot_idx);
    return 0;
}

int vfs_hd_mounted(void)
{
    for (int i = 0; i < s_nmounts; i++) {
        vfs_backend_t b = s_mounts[i].backend;
        if (b == VFS_BACKEND_EXT2 || b == VFS_BACKEND_FAT32) return 1;
    }
    return 0;
}

const char *vfs_hd_fsname(const char *name)
{
    int mi = name ? mount_find_slot(name) : -1;
    return (mi >= 0) ? backend_name(s_mounts[mi].backend) : "none";
}

void vfs_print_mounts(void)
{
    int any = 0;
    for (int i = 0; i < s_nmounts; i++) {
        vfs_mount_t *m = &s_mounts[i];
        if (m->backend == VFS_BACKEND_NONE) continue;

        any = 1;
        t_writestring(m->mountpoint);
        t_writestring(" type ");
        t_writestring(backend_name(m->backend));
        if (m->backend == VFS_BACKEND_EXT2 ||
            m->backend == VFS_BACKEND_FAT32 ||
            m->backend == VFS_BACKEND_ISO9660) {
            t_writestring(" drive ");
            t_dec(m->drive);
            if (m->backend != VFS_BACKEND_ISO9660) {
                t_writestring(" lba ");
                t_dec(m->lba);
            }
        }
        t_putchar('\n');
    }
    if (!any)
        t_writestring("(no mounted filesystems)\n");
}

void vfs_prepare_shutdown(void)
{
    /* Flush every HD-backed mount on the way down. */
    for (int i = s_nmounts - 1; i >= 0; i--) {
        vfs_backend_t b = s_mounts[i].backend;
        if (b != VFS_BACKEND_EXT2 && b != VFS_BACKEND_FAT32) continue;
        t_writestring("Syncing ");
        t_writestring(s_mounts[i].mountpoint);
        t_writestring(" ...\n");
        if (!mount_has_alias(i))
            backend_unmount(b);
        s_mounts[i].backend = VFS_BACKEND_NONE;
    }
}

void vfs_notify_cdrom_ejected(void)
{
    s_cdrom_drive = -1;
    /* Drop every entry pointing at the CD-ROM (typically /mnt/cdrom
     * and possibly "/"). */
    for (int i = s_nmounts - 1; i >= 0; i--) {
        if (s_mounts[i].backend == VFS_BACKEND_ISO9660) {
            apply_cwd_fixup(fixup_cwd_unmounted, s_mounts[i].mountpoint);
            mount_remove(i);
        }
    }
}

/* -------------------------------------------------------------------------
 * Listing: ls / + ls /mnt
 *
 * ls /     -- walk every mount whose mountpoint is one component deep
 *             (i.e. immediate child of /).  Plus the rootfs's own
 *             directory contents if a backend is bound at /.
 * ls /mnt  -- walk every mount whose mountpoint matches /mnt/<one comp>.
 * ---------------------------------------------------------------------- */

/* True if `mp` is exactly one path component below `parent`/.  E.g.
 * is_immediate_child("/mnt/cdrom", "/mnt") -> 1; "/mnt/foo/bar" -> 0. */
static int is_immediate_child(const char *mp, const char *parent)
{
    size_t pn = strlen(parent);
    if (strncmp(mp, parent, pn) != 0) return 0;
    if (mp[pn] != '/') return 0;
    const char *rest = mp + pn + 1;
    if (!*rest) return 0;
    for (const char *q = rest; *q; q++) if (*q == '/') return 0;
    return 1;
}

static void ls_root(void)
{
    /* List immediate-child mount entries (overlays + /boot promotion +
     * any other mount whose path is "/X"). */
    int root_idx = mount_find_exact("/");
    for (int i = 0; i < s_nmounts; i++) {
        if (i == root_idx) continue;
        const char *mp = s_mounts[i].mountpoint;
        if (mp[0] != '/' || strcmp(mp, "/") == 0) continue;
        /* Only one-component-deep entries appear in /. */
        const char *rest = mp + 1;
        for (const char *q = rest; *q; q++) if (*q == '/') { rest = NULL; break; }
        if (!rest) continue;
        t_putchar('[');
        t_writestring(rest);
        t_putchar(']');
        t_putchar('\n');
    }
    /* If a rootfs is bound at "/", also list its actual directory contents. */
    if (root_idx >= 0)
        (void)backend_ls(&s_mounts[root_idx], "/");
}

static void ls_mnt(void)
{
    int shown = 0;
    for (int i = 0; i < s_nmounts; i++) {
        if (!is_immediate_child(s_mounts[i].mountpoint, "/mnt")) continue;
        t_putchar('[');
        t_writestring(s_mounts[i].slot_name[0]
                          ? s_mounts[i].slot_name
                          : s_mounts[i].mountpoint + 5);
        t_putchar(']');
        if (!mount_is_bound(i))
            t_writestring("  (empty mountpoint)");
        else {
            t_writestring("  ");
            t_writestring(backend_name(s_mounts[i].backend));
        }
        t_putchar('\n');
        shown++;
    }
    if (shown == 0)
        t_writestring("(no mountpoints - use 'mkdir /mnt/<name>' then 'mount')\n");
}

/* -------------------------------------------------------------------------
 * vfs_ls / vfs_cd / vfs_cat
 * ---------------------------------------------------------------------- */

int vfs_ls(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);
    const char *drv;
    int idx = vfs_route(abs, &drv);
    /* Special-case virtual /: even if a real rootfs is mounted, we
     * union the overlays into the listing. */
    if (strcmp(abs, "/") == 0) { ls_root(); return 0; }
    if (strcmp(abs, "/mnt") == 0) { ls_mnt(); return 0; }
    if (idx < 0) { t_writestring("ls: path not found\n"); return -1; }
    if (s_mounts[idx].backend == VFS_BACKEND_NONE) {
        t_writestring("(empty mountpoint - mount a filesystem here)\n");
        return 0;
    }
    return backend_ls(&s_mounts[idx], drv);
}

int vfs_cd(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);
    const char *drv;
    int idx = vfs_route(abs, &drv);
    char *cwd = cwd_buf();

    /* Virtual root / /mnt / empty mountpoints are valid directories. */
    if (strcmp(abs, "/") == 0 || strcmp(abs, "/mnt") == 0 ||
        (idx >= 0 && s_mounts[idx].backend == VFS_BACKEND_NONE)) {
        memcpy(cwd, abs, strlen(abs) + 1);
        return 0;
    }
    if (idx < 0) { t_writestring("cd: path not found\n"); return -1; }

    vfs_mount_t *m = &s_mounts[idx];
    switch (m->backend) {
    case VFS_BACKEND_EXT2:
    case VFS_BACKEND_FAT32:
        if (backend_cd(m, drv) != 0) {
            t_writestring("cd: directory not found\n"); return -1;
        }
        strncpy(cwd, abs, VFS_PATH_MAX - 1); cwd[VFS_PATH_MAX - 1] = '\0';
        return 0;
    case VFS_BACKEND_ISO9660:
        /* No cheap directory check; optimistically accept. */
        strncpy(cwd, abs, VFS_PATH_MAX - 1); cwd[VFS_PATH_MAX - 1] = '\0';
        return 0;
    case VFS_BACKEND_PROCFS:
    case VFS_BACKEND_DEVFS:
    case VFS_BACKEND_LOGFS:
    case VFS_BACKEND_TMPFS:
        /* Synthetic backends are flat: only the mount root itself is a dir. */
        if (drv[0] == '/' && drv[1] == '\0') {
            strncpy(cwd, abs, VFS_PATH_MAX - 1); cwd[VFS_PATH_MAX - 1] = '\0';
            return 0;
        }
        t_writestring("cd: not a directory\n"); return -1;
    default:
        t_writestring("cd: path not found\n"); return -1;
    }
}

int vfs_cat(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);
    const char *drv;
    int idx = vfs_route(abs, &drv);
    if (idx < 0 || s_mounts[idx].backend == VFS_BACKEND_NONE) {
        t_writestring("cat: not a file\n"); return -1;
    }
    enum { CAT_MAX = 64u * 1024u };
    uint8_t *buf = (uint8_t *)kmalloc(CAT_MAX);
    if (!buf) { t_writestring("cat: out of memory\n"); return -1; }
    uint32_t got = 0;
    int err = backend_read_file(&s_mounts[idx], drv, buf, CAT_MAX, &got);
    if (err) { t_writestring("cat: file not found\n"); kfree(buf); return -1; }
    t_write((const char *)buf, got);
    if (got > 0 && buf[got - 1] != '\n') t_putchar('\n');
    kfree(buf);
    return 0;
}

/* -------------------------------------------------------------------------
 * vfs_mkdir: mkdir /mnt/<name> creates an empty mountpoint; deeper
 * paths create a directory on the bound backend.
 * ---------------------------------------------------------------------- */

/* Yields <name> if abs is "/mnt/<name>" exactly (one component below
 * /mnt); returns 1 on match, 0 otherwise. */
static int mnt_leaf(const char *abs, char *out, size_t outsz)
{
    if (strncmp(abs, "/mnt/", 5) != 0) return 0;
    const char *name = abs + 5;
    if (!*name) return 0;
    size_t i = 0;
    for (const char *q = name; *q; q++) {
        if (*q == '/') return 0;
        if (i + 1 >= outsz) return 0;
        out[i++] = *q;
    }
    out[i] = '\0';
    return 1;
}

int vfs_mkdir(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);

    /* mkdir /mnt/<name> -> create empty mountpoint. */
    char mpname[VFS_MOUNT_NAME_MAX];
    if (mnt_leaf(abs, mpname, sizeof(mpname))) {
        int r = vfs_make_mountpoint(mpname);
        switch (r) {
        case 0:   return 0;
        case -6:  t_writestring("mkdir: already exists: "); t_writestring(abs); t_putchar('\n'); break;
        case -12: t_writestring("mkdir: mountpoint table full (max ");
                  t_dec(MAX_MOUNTS); t_writestring(")\n"); break;
        case -13: t_writestring("mkdir: 'cdrom' is reserved\n"); break;
        default:  t_writestring("mkdir: bad mountpoint name: "); t_writestring(abs); t_putchar('\n'); break;
        }
        return -1;
    }

    const char *drv;
    int idx = vfs_route(abs, &drv);
    if (idx < 0 || s_mounts[idx].backend == VFS_BACKEND_NONE) {
        t_writestring("mkdir: cannot create directory here\n"); return -1;
    }
    if (s_mounts[idx].backend == VFS_BACKEND_ISO9660) {
        t_writestring("mkdir: read-only filesystem\n"); return -1;
    }
    return backend_mkdir(&s_mounts[idx], drv);
}

/* -------------------------------------------------------------------------
 * vfs_read_file / vfs_write_file
 * ---------------------------------------------------------------------- */

int vfs_read_file(const char *path, void *buf, uint32_t bufsz, uint32_t *out_sz)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);
    const char *drv;
    int idx = vfs_route(abs, &drv);
    if (idx < 0 || s_mounts[idx].backend == VFS_BACKEND_NONE) return -1;
    return backend_read_file(&s_mounts[idx], drv, buf, bufsz, out_sz);
}

int vfs_write_file(const char *path, const void *buf, uint32_t size)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);
    const char *drv;
    int idx = vfs_route(abs, &drv);
    if (idx < 0 || s_mounts[idx].backend == VFS_BACKEND_NONE) return -1;
    return backend_write_file(&s_mounts[idx], drv, buf, size);
}

int vfs_delete_file(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);
    const char *drv;
    int idx = vfs_route(abs, &drv);
    if (idx < 0 || s_mounts[idx].backend == VFS_BACKEND_NONE) return -1;
    return backend_delete_file(&s_mounts[idx], drv);
}

int vfs_delete_dir(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);
    /* rmdir /mnt/<name> -> remove empty mountpoint. */
    char mpname[VFS_MOUNT_NAME_MAX];
    if (mnt_leaf(abs, mpname, sizeof(mpname))) {
        int r = vfs_remove_mountpoint(mpname);
        if (r == -16) {
            t_writestring("rmdir: mountpoint busy - umount first\n");
            return -1;
        }
        return r;
    }
    const char *drv;
    int idx = vfs_route(abs, &drv);
    if (idx < 0 || s_mounts[idx].backend == VFS_BACKEND_NONE) return -1;
    return backend_delete_dir(&s_mounts[idx], drv);
}

int vfs_rename(const char *old_path, const char *new_path)
{
    char old_abs[VFS_PATH_MAX], new_abs[VFS_PATH_MAX];
    path_resolve(old_path, old_abs);
    path_resolve(new_path, new_abs);
    const char *old_drv, *new_drv;
    int oi = vfs_route(old_abs, &old_drv);
    int ni = vfs_route(new_abs, &new_drv);
    if (oi < 0 || ni < 0) return -1;
    if (s_mounts[oi].backend != s_mounts[ni].backend) return -1;
    if (s_mounts[oi].backend == VFS_BACKEND_EXT2)
        return ext2_file_exists(old_drv) ? ext2_rename_file(old_drv, new_drv)
                                          : ext2_rename_dir(old_drv, new_drv);
    if (s_mounts[oi].backend == VFS_BACKEND_FAT32)
        return fat32_file_exists(old_drv) ? fat32_rename_file(old_drv, new_drv)
                                           : fat32_rename_dir(old_drv, new_drv);
    return -1;
}

int vfs_file_exists(const char *path)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);
    const char *drv;
    int idx = vfs_route(abs, &drv);
    if (idx < 0 || s_mounts[idx].backend == VFS_BACKEND_NONE) return 0;
    return backend_file_exists(&s_mounts[idx], drv);
}

/* -------------------------------------------------------------------------
 * vfs_stat: per-backend size + kind probe (no eager-load).
 * ---------------------------------------------------------------------- */
int vfs_stat(const char *path, vfs_stat_info_t *out)
{
    if (!out) return -1;
    out->size = 0; out->kind = VFS_STAT_FILE;
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);

    /* Virtual / and /mnt always exist as directories. */
    if (strcmp(abs, "/") == 0 || strcmp(abs, "/mnt") == 0) {
        out->kind = VFS_STAT_DIR; return 0;
    }
    const char *drv;
    int idx = vfs_route(abs, &drv);
    if (idx < 0) return -1;
    vfs_mount_t *m = &s_mounts[idx];

    if (m->backend == VFS_BACKEND_NONE) { out->kind = VFS_STAT_DIR; return 0; }

    /* Mountpoint root itself is a directory. */
    if (drv[0] == '/' && drv[1] == '\0' &&
        strcmp(abs, m->mountpoint) == 0) {
        out->kind = VFS_STAT_DIR; return 0;
    }

    switch (m->backend) {
    case VFS_BACKEND_EXT2: {
        uint32_t sz = 0; int isdir = 0;
        if (ext2_stat(drv, &sz, &isdir) != 0) return -1;
        out->size = sz; out->kind = isdir ? VFS_STAT_DIR : VFS_STAT_FILE;
        return 0;
    }
    case VFS_BACKEND_FAT32: {
        uint32_t sz = 0;
        if (fat32_read_file(drv, NULL, 0xFFFFFFFFu, &sz) == 0) {
            out->size = sz; out->kind = VFS_STAT_FILE; return 0;
        }
        return -1;
    }
    case VFS_BACKEND_ISO9660: {
        uint32_t sz = 0;
        if (iso9660_read_file(m->drive, drv, NULL, 0xFFFFFFFFu, &sz) == 0) {
            out->size = sz; out->kind = VFS_STAT_FILE; return 0;
        }
        return -1;
    }
    case VFS_BACKEND_PROCFS: {
        if (!procfs_file_exists(drv)) return -1;
        uint8_t *s = (uint8_t *)kmalloc(4096);
        if (!s) return -1;
        uint32_t got = 0;
        int rc = procfs_read_file(drv, s, 4096, &got);
        kfree(s);
        if (rc != 0) return -1;
        out->size = got; out->kind = VFS_STAT_FILE; return 0;
    }
    case VFS_BACKEND_LOGFS: {
        uint8_t *s = (uint8_t *)kmalloc(64u * 1024u);
        if (!s) return -1;
        uint32_t got = 0;
        int rc = (logfs_read(drv, s, 64u * 1024u, &got) < 0) ? -1 : 0;
        kfree(s);
        if (rc != 0) return -1;
        out->size = got; out->kind = VFS_STAT_FILE; return 0;
    }
    case VFS_BACKEND_TMPFS: {
        long sz = tmpfs_size(drv);
        if (sz < 0) return -1;
        out->size = (uint32_t)sz; out->kind = VFS_STAT_FILE; return 0;
    }
    case VFS_BACKEND_DEVFS: {
        uint32_t dev_sz = 0;
        int node = vfs_blockdev_lookup(abs, &dev_sz);
        if (node < 0) return -1;
        out->size = dev_sz; out->kind = VFS_STAT_BLOCKDEV; return 0;
    }
    default: return -1;
    }
}

int vfs_blockdev_lookup(const char *path, uint32_t *size_out)
{
    char abs[VFS_PATH_MAX];
    path_resolve(path, abs);
    const char *drv;
    int idx = vfs_route(abs, &drv);
    if (idx < 0 || s_mounts[idx].backend != VFS_BACKEND_DEVFS) return -1;
    int node = devfs_lookup(drv);
    if (node < 0) return -1;
    if (size_out) *size_out = devfs_node_size(node);
    return node;
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
 * vfs_complete -- tab-completion enumerator
 * ---------------------------------------------------------------------- */
int vfs_complete(const char *dir, const char *prefix,
                 fat32_complete_cb_t cb, void *ctx)
{
    char abs[VFS_PATH_MAX];
    path_resolve(dir ? dir : cwd_buf(), abs);
    if (strcmp(abs, "/") == 0) {
        /* Enumerate immediate-child mount points + the rootfs contents. */
        if (cb) {
            for (int i = 0; i < s_nmounts; i++) {
                const char *mp = s_mounts[i].mountpoint;
                if (mp[0] != '/' || strcmp(mp, "/") == 0) continue;
                const char *rest = mp + 1;
                int deep = 0;
                for (const char *q = rest; *q; q++) if (*q == '/') { deep = 1; break; }
                if (!deep) cb(rest, 1, ctx);
            }
        }
        int root_idx = mount_find_exact("/");
        if (root_idx >= 0) return backend_complete(&s_mounts[root_idx], "/", prefix, cb, ctx);
        return 0;
    }
    if (strcmp(abs, "/mnt") == 0) {
        if (cb) {
            for (int i = 0; i < s_nmounts; i++)
                if (is_immediate_child(s_mounts[i].mountpoint, "/mnt"))
                    cb(s_mounts[i].slot_name[0]
                           ? s_mounts[i].slot_name
                           : s_mounts[i].mountpoint + 5, 1, ctx);
        }
        return 0;
    }
    const char *drv;
    int idx = vfs_route(abs, &drv);
    if (idx < 0 || s_mounts[idx].backend == VFS_BACKEND_NONE) return -1;
    return backend_complete(&s_mounts[idx], drv, prefix, cb, ctx);
}
