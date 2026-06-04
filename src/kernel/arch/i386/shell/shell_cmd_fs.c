/*
 * shell_cmd_fs.c -- filesystem shell builtins.
 *
 * Builtins live in the shell because they mutate parent state (cd) or are
 * cheap kernel-side ops with no userland equivalent yet.  ls/cat/cp/mv/rm
 * are external -- they ship as applets in fsutil.elf and are reached via
 * the shell's PATH-lookup -> fsutil-fallback chain in shell.c.
 */

#include "shell_priv.h"

#include <kernel/tty.h>
#include <kernel/fat32.h>
#include <kernel/ext2.h>
#include <kernel/vfs.h>
#include <kernel/iso9660.h>
#include <kernel/partition.h>
#include <kernel/ide.h>
#include <kernel/devfs.h>
#include <kernel/admin.h>
#include <kernel/unzip.h>
#include <kernel/heap.h>
#include <string.h>

static disk_parts_t s_cmd_parts;

/* Validate a mountpoint of the form "/mnt/<name>" and return <name>
 * (a single component directly under /mnt), or NULL if malformed.
 * /mnt/cdrom is reserved for the optical drive. */
static const char *mountpoint_name(const char *mp)
{
    if (strncmp(mp, "/mnt/", 5) != 0) return NULL;
    const char *name = mp + 5;
    if (*name == '\0') return NULL;          /* "/mnt" itself              */
    for (const char *q = name; *q; q++)
        if (*q == '/') return NULL;          /* must be one level deep     */
    if (strcmp(name, "cdrom") == 0) return NULL;  /* reserved              */
    return name;
}

/* admin_mount -- bind a /dev/hdaN block device at /mnt/<name>.
 * Both args required (NULL/empty dev_path means "print mounts and
 * return 0", matching `mount` with no args).  Prints the same
 * diagnostics on failure as the previous cmd_mount did, so the
 * userspace shell gets identical UX. */
int admin_mount(const char *dev_path, const char *mnt_path)
{
    if (!dev_path || !*dev_path) {
        vfs_print_mounts();
        return 0;
    }
    if (!mnt_path || strncmp(dev_path, "/dev/", 5) != 0) {
        t_writestring("Usage: mount [ /dev/hdaN /mnt/<name> ]\n");
        return -1;
    }
    int node = devfs_lookup(dev_path + 4);   /* "/dev/hda1" -> "/hda1" */
    if (node < 0) {
        t_writestring("mount: no such device: ");
        t_writestring(dev_path);
        t_putchar('\n');
        return -1;
    }
    const char *mount_name = mountpoint_name(mnt_path);
    if (!mount_name) {
        t_writestring("mount: bad mountpoint '");
        t_writestring(mnt_path);
        t_writestring("' (expected /mnt/<name>, one level deep; "
                      "cdrom is reserved)\n");
        return -1;
    }
    uint8_t  drive;
    uint32_t lba;
    if (devfs_node_location(node, &drive, &lba) != 0) {
        t_writestring("mount: cannot resolve device geometry\n");
        return -1;
    }
    int fs = 0;
    int err = vfs_mount_hd(drive, lba, mount_name, &fs);
    if (err) {
        switch (err) {
        case -10: t_writestring("mount: /mnt/"); t_writestring(mount_name);
                  t_writestring(" already mounted\n"); break;
        case -11: t_writestring("mount: that filesystem type is already "
                                "mounted elsewhere (one FAT32 + one ext2 max)\n"); break;
        case -12: t_writestring("mount: mount table full\n"); break;
        case -13: t_writestring("mount: 'cdrom' is reserved\n"); break;
        case -14: t_writestring("mount: no such mountpoint /mnt/"); t_writestring(mount_name);
                  t_writestring(" - create it first with 'mkdir /mnt/"); t_writestring(mount_name);
                  t_writestring("'\n"); break;
        default:  t_writestring("mount: not a recognised FAT32 or ext2 volume "
                                "(error "); t_dec((uint32_t)(-err));
                  t_writestring(")\n"); break;
        }
        return err;
    }
    t_writestring("Mounted ");
    t_writestring(vfs_hd_fsname(mount_name));
    t_writestring("  drive ");   t_dec(drive);
    t_writestring("  LBA ");     t_dec(lba);
    t_writestring("  at /mnt/"); t_writestring(mount_name);
    t_writestring("\ncwd: ");    t_writestring(vfs_getcwd());
    t_putchar('\n');
    return 0;
}

/* mount /dev/hdaN /mnt/<name>  -- the only supported form. */
static void cmd_mount(int argc, char **argv)
{
    admin_mount(argc >= 2 ? argv[1] : NULL,
                argc >= 3 ? argv[2] : NULL);
}

/* True if `target` names the CD-ROM mount (/mnt/cdrom, or bare "cdrom"). */
static int umount_target_is_cdrom(const char *t)
{
    return strcmp(t, "/mnt/cdrom") == 0 || strcmp(t, "cdrom") == 0;
}

/* admin_eject -- unmount the optical drive (if any) and open the tray.
 * Returns 0 on success, -1 if no ATAPI drive is present, or the negative
 * ATAPI error code on hardware failure.  Shared between cmd_umount's
 * "/mnt/cdrom" path and SYS_EJECT from the userspace shell. */
int admin_eject(void)
{
    int cd_drive = -1;
    for (int i = 0; i < IDE_MAX_DRIVES; i++) {
        const ide_drive_t *d = ide_get_drive((uint8_t)i);
        if (d && d->present && d->type == IDE_TYPE_ATAPI) { cd_drive = i; break; }
    }
    if (cd_drive < 0) {
        t_writestring("eject: no CD-ROM drive detected\n");
        return -1;
    }
    vfs_notify_cdrom_ejected();
    int err = ide_eject_atapi((uint8_t)cd_drive);
    if (err) {
        t_writestring("eject: ATAPI eject failed (err ");
        t_dec((uint32_t)err);
        t_writestring(")\n");
        return err;
    }
    t_writestring("CD-ROM unmounted and ejected.\n");
    return 0;
}

/* admin_umount -- unmount /mnt/<name>; target NULL/empty means the sole
 * HD mount (errors -20 if ambiguous).  Special-cases /mnt/cdrom by
 * delegating to admin_eject.  Returns 0 on success, negative errno. */
int admin_umount(const char *target)
{
    if (target && umount_target_is_cdrom(target))
        return admin_eject();

    const char *name = NULL;
    if (target && *target)
        name = (strncmp(target, "/mnt/", 5) == 0) ? target + 5 : target;
    int err = vfs_umount_hd(name);
    if (err == -20) { t_writestring("umount: multiple volumes mounted - specify /mnt/<name>\n"); return err; }
    if (err == -15) { t_writestring("umount: nothing mounted at that mountpoint\n");            return err; }
    if (err)        { t_writestring("umount: no such mount\n");                                 return err; }
    t_writestring("Volume unmounted.\n");
    return 0;
}

/* umount [/mnt/<name>]   default target is the FAT32 volume.
 * umount /mnt/cdrom      eject the optical drive. */
static void cmd_umount(int argc, char **argv)
{
    admin_umount(argc >= 2 ? argv[1] : NULL);
}

static void cmd_cd(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring(vfs_getcwd());
        t_putchar('\n');
        return;
    }
    if (vfs_cd(argv[1]) == 0) {
        t_writestring(vfs_getcwd());
        t_putchar('\n');
    }
}

static void cmd_mkdir(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("Usage: mkdir <path>...\n");
        return;
    }
    int any_fail = 0;
    for (int i = 1; i < argc; i++) {
        int err = vfs_mkdir(argv[i]);
        switch (err) {
        case  0: break;
        case -1: t_writestring("mkdir: cannot create '");  t_writestring(argv[i]); t_writestring("'\n"); any_fail = 1; break;
        case -2: t_writestring("mkdir: I/O error: '");     t_writestring(argv[i]); t_writestring("'\n"); any_fail = 1; break;
        case -4: t_writestring("mkdir: disk full: '");     t_writestring(argv[i]); t_writestring("'\n"); any_fail = 1; break;
        case -6: t_writestring("mkdir: already exists: '");t_writestring(argv[i]); t_writestring("'\n"); any_fail = 1; break;
        default:
            if (err < 0) {
                t_writestring("mkdir: error ");
                t_dec((uint32_t)(-err));
                t_writestring(": ");
                t_writestring(argv[i]);
                t_putchar('\n');
                any_fail = 1;
            }
            break;
        }
    }
    if (any_fail) shell_set_last_exec_status(1);
}

static void cmd_mkfs(int argc, char **argv)
{
    if (argc < 3) {
        t_writestring("Usage: mkfs <drive> <part>\n");
        return;
    }

    uint8_t drive    = (uint8_t)parse_uint(argv[1]);
    int     part_num = (int)parse_uint(argv[2]);

    int err = part_probe(drive, &s_cmd_parts);
    if (err) {
        t_writestring("mkfs: drive not accessible\n");
        return;
    }

    int part_idx = part_num - 1;
    if (part_idx < 0 || part_idx >= s_cmd_parts.count) {
        t_writestring("mkfs: invalid partition number");
        if (s_cmd_parts.count > 0) {
            t_writestring(" (valid: 1-");
            t_dec((uint32_t)s_cmd_parts.count);
            t_putchar(')');
        }
        t_writestring("\n      (use lspart ");
        t_dec(drive);
        t_writestring(" to list partitions)\n");
        return;
    }

    uint32_t lba     = s_cmd_parts.parts[part_idx].lba_start;
    uint32_t sectors = s_cmd_parts.parts[part_idx].lba_count;

    t_writestring("Formatting drive ");
    t_dec(drive);
    t_writestring(" partition ");
    t_dec((uint32_t)part_num);
    t_writestring(" (");
    t_dec(sectors / 2048u);
    t_writestring(" MiB) as FAT32...\n");

    err = fat32_mkfs(drive, lba, sectors);
    if (err == -6) {
        t_writestring("mkfs: partition too small for FAT32 (need >= 32 MiB)\n");
        return;
    }
    if (err) {
        t_writestring("mkfs: I/O error (");
        t_dec((uint32_t)(-err));
        t_writestring(")\n");
        return;
    }

    t_writestring("Done.  Mount with: mount ");
    t_dec(drive);
    t_putchar(' ');
    t_dec((uint32_t)part_num);
    t_putchar('\n');
}

/* Resolve a /dev/hdaN path to (drive, lba, sectors).  Returns 0 on success. */
static int resolve_dev(const char *path, uint8_t *drive, uint32_t *lba,
                       uint32_t *sectors)
{
    if (strncmp(path, "/dev/", 5) != 0) return -1;
    int node = devfs_lookup(path + 4);
    if (node < 0) return -1;
    if (devfs_node_location(node, drive, lba) != 0) return -1;
    *sectors = devfs_node_size(node) / 512u;
    return 0;
}

/* admin_mkfs -- format /dev/hdaN with `fstype` ("ext2" or "fat32").
 * Returns 0 on success, -1 on bad args, or negative mkfs error code. */
int admin_mkfs(const char *dev_path, const char *fstype)
{
    int ext2;
    if (!dev_path || !fstype) { t_writestring("Usage: mkfs.ext2|mkfs.fat32 /dev/hdaN\n"); return -1; }
    if (strcmp(fstype, "ext2") == 0)       ext2 = 1;
    else if (strcmp(fstype, "fat32") == 0) ext2 = 0;
    else {
        t_writestring("mkfs: unknown filesystem type (expected ext2 or fat32)\n");
        return -1;
    }
    uint8_t drive; uint32_t lba, sectors;
    if (resolve_dev(dev_path, &drive, &lba, &sectors) != 0) {
        t_writestring("mkfs: no such device (expected /dev/hdaN)\n");
        return -1;
    }
    t_writestring("Formatting ");
    t_writestring(dev_path);
    t_writestring(" (");
    t_dec(sectors / 2048u);
    t_writestring(ext2 ? " MiB) as ext2...\n" : " MiB) as FAT32...\n");

    int err = ext2 ? ext2_mkfs(drive, lba, sectors)
                   : fat32_mkfs(drive, lba, sectors);
    if (err == -6) { t_writestring("mkfs: partition too small\n"); return err; }
    if (err)       { t_writestring("mkfs: I/O error\n");           return err; }
    t_writestring("Done.  Mount with: mount ");
    t_writestring(dev_path);
    t_writestring(" /mnt/<name>\n");
    return 0;
}

static void cmd_mkfs_ext2(int argc, char **argv)
{
    if (argc < 2) { t_writestring("Usage: mkfs.ext2 /dev/hdaN\n"); return; }
    admin_mkfs(argv[1], "ext2");
}

static void cmd_mkfs_fat32(int argc, char **argv)
{
    if (argc < 2) { t_writestring("Usage: mkfs.fat32 /dev/hdaN\n"); return; }
    admin_mkfs(argv[1], "fat32");
}

static void cmd_isols(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("Usage: isols <drive> [path]\n");
        return;
    }

    uint8_t drive = (uint8_t)parse_uint(argv[1]);
    const char *path = (argc >= 3) ? argv[2] : "/";

    int err = iso9660_ls(drive, path);
    if (err == -2)
        t_writestring("isols: path not found or not ISO9660\n");
    else if (err)
        t_writestring("isols: I/O error\n");
}

static void cmd_write(int argc, char **argv)
{
    if (argc < 3) {
        t_writestring("Usage: write <file> <text...>\n");
        return;
    }

    static char s_write_buf[SHELL_MAX_INPUT];
    size_t off = 0;

    for (int i = 2; i < argc; i++) {
        if (i > 2 && off < sizeof(s_write_buf) - 1)
            s_write_buf[off++] = ' ';
        const char *s = argv[i];
        while (*s && off < sizeof(s_write_buf) - 1)
            s_write_buf[off++] = *s++;
    }
    if (off < sizeof(s_write_buf) - 1)
        s_write_buf[off++] = '\n';
    s_write_buf[off] = '\0';

    static char s_write_path[VFS_PATH_MAX];
    const char *arg = argv[1];
    const char *cwd = vfs_getcwd();
    if (arg[0] == '/') {
        strncpy(s_write_path, arg, VFS_PATH_MAX - 1);
        s_write_path[VFS_PATH_MAX - 1] = '\0';
    } else {
        size_t cl = strlen(cwd), al = strlen(arg);
        if (cl + 1 + al >= VFS_PATH_MAX) { t_writestring("write: path too long\n"); return; }
        size_t p = 0;
        memcpy(s_write_path, cwd, cl); p += cl;
        if (cwd[cl - 1] != '/') s_write_path[p++] = '/';
        memcpy(s_write_path + p, arg, al + 1);
    }

    int err = vfs_write_file(s_write_path, s_write_buf, (uint32_t)off);
    if (err) {
        t_writestring("write: error ");
        t_dec((uint32_t)(-err));
        t_putchar('\n');
    }
}

static void cmd_touch(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("Usage: touch <file>...\n");
        return;
    }

    static char s_touch_path[VFS_PATH_MAX];
    const char *cwd = vfs_getcwd();

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] == '/') {
            strncpy(s_touch_path, arg, VFS_PATH_MAX - 1);
            s_touch_path[VFS_PATH_MAX - 1] = '\0';
        } else {
            size_t cl = strlen(cwd), al = strlen(arg);
            if (cl + 1 + al >= VFS_PATH_MAX) {
                t_writestring("touch: path too long: ");
                t_writestring(arg);
                t_putchar('\n');
                continue;
            }
            size_t p = 0;
            memcpy(s_touch_path, cwd, cl); p += cl;
            if (cwd[cl - 1] != '/') s_touch_path[p++] = '/';
            memcpy(s_touch_path + p, arg, al + 1);
        }

        int err = vfs_write_file(s_touch_path, "", 0);
        if (err) {
            t_writestring("touch: error ");
            t_dec((uint32_t)(-err));
            t_writestring(": ");
            t_writestring(arg);
            t_putchar('\n');
        }
    }
}

static void cmd_unzip(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("usage: unzip <archive.zip> [destdir]\n");
        return;
    }
    const char *zippath = argv[1];
    const char *destdir = (argc >= 3) ? argv[2] : vfs_getcwd();

    vfs_stat_info_t st;
    if (vfs_stat(zippath, &st) != 0 || st.kind != VFS_STAT_FILE) {
        t_writestring("unzip: cannot stat archive\n");
        return;
    }
    if (st.size == 0) {
        t_writestring("unzip: empty archive\n");
        return;
    }

    uint8_t *buf = (uint8_t *)kmalloc(st.size);
    if (!buf) {
        t_writestring("unzip: out of memory\n");
        return;
    }
    uint32_t got = 0;
    if (vfs_read_file(zippath, buf, st.size, &got) != 0 || got != st.size) {
        t_writestring("unzip: read failed\n");
        kfree(buf);
        return;
    }

    int failed = 0;
    int n = unzip_archive(buf, got, destdir, &failed);
    kfree(buf);

    if (n < 0) {
        t_writestring("unzip: not a valid zip archive\n");
        return;
    }
    t_writestring("Extracted ");
    t_dec((uint32_t)n);
    t_writestring(" file(s) to ");
    t_writestring(destdir);
    if (failed) {
        t_writestring(" (");
        t_dec((uint32_t)failed);
        t_writestring(" failed/skipped)");
    }
    t_writestring("\n");
}

const shell_cmd_entry_t fs_cmds[] = {
    { "mount",  cmd_mount  },
    { "umount", cmd_umount },
    { "cd",     cmd_cd     },
    { "mkdir",  cmd_mkdir  },
    { "mkfs",       cmd_mkfs       },
    { "mkfs.ext2",  cmd_mkfs_ext2  },
    { "mkfs.fat32", cmd_mkfs_fat32 },
    { "isols",  cmd_isols  },
    { "write",  cmd_write  },
    { "touch",  cmd_touch  },
    { "unzip",  cmd_unzip  },
    { NULL, NULL }
};
