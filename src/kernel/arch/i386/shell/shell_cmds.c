/*
 * shell_cmds.c -- built-in shell command implementations.
 *
 * Every command handler declared in shell_priv.h (except cmd_help and
 * cmd_version, which live in shell_help.c) is implemented here.
 */

#include "shell_priv.h"

#include <kernel/tty.h>
#include <kernel/timer.h>
#include <kernel/heap.h>
#include <kernel/vesa_tty.h>
#include <kernel/ide.h>
#include <kernel/partition.h>
#include <kernel/fat32.h>
#include <kernel/vfs.h>
#include <kernel/iso9660.h>
#include <kernel/installer.h>
#include <kernel/task.h>
#include <kernel/acpi.h>
#include <kernel/vics.h>

#include <string.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Private helpers
 * --------------------------------------------------------------------------- */

/* Parse a simple decimal or 0x-prefixed hex number from a string. */
static uint32_t parse_uint(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        uint32_t v = 0;
        s += 2;
        while (*s) {
            char c = *s++;
            if (c >= '0' && c <= '9')      v = v * 16 + (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v = v * 16 + (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = v * 16 + (uint32_t)(c - 'A' + 10);
            else break;
        }
        return v;
    }
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

/* Simple hex dump of a 512-byte buffer: 32 rows of 16 bytes. */
static void hexdump_sector(const uint8_t *buf)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int row = 0; row < 32; row++) {
        int offset = row * 16;
        /* Offset */
        t_putchar(hex[(offset >> 8) & 0xF]);
        t_putchar(hex[(offset >> 4) & 0xF]);
        t_putchar(hex[(offset     ) & 0xF]);
        t_putchar('0');
        t_writestring(":  ");

        for (int col = 0; col < 16; col++) {
            uint8_t b = buf[offset + col];
            t_putchar(hex[b >> 4]);
            t_putchar(hex[b & 0xF]);
            t_putchar(' ');
        }
        t_putchar('\n');
    }
}

/* ---------------------------------------------------------------------------
 * Shared partition table storage for lspart / mkpart.
 * Stored as a static so it never lands on the kernel stack.
 * --------------------------------------------------------------------------- */
static disk_parts_t s_disk_parts;

/* Print one probed disk_parts_t to the terminal. */
static void disk_parts_print(const disk_parts_t *dp)
{
    static const char hex[] = "0123456789ABCDEF";

    if (dp->scheme == PART_SCHEME_NONE) {
        t_writestring("No partition table found on this drive.\n");
        return;
    }

    t_writestring("Scheme: ");
    t_writestring(dp->scheme == PART_SCHEME_GPT ? "GPT" : "MBR");
    t_writestring("  total sectors: ");
    t_dec(dp->total_sectors);
    t_writestring("  (");
    t_dec(dp->total_sectors / 2048);
    t_writestring(" MiB)\n");

    if (dp->count == 0) {
        t_writestring("No partitions.\n");
        return;
    }

    for (int i = 0; i < dp->count; i++) {
        const part_info_t *p = &dp->parts[i];

        t_writestring("  [");
        t_dec((uint32_t)(i + 1));
        t_writestring("] ");

        if (dp->scheme == PART_SCHEME_MBR) {
            t_writestring("type=0x");
            t_putchar(hex[p->mbr_type >> 4]);
            t_putchar(hex[p->mbr_type & 0xF]);
            t_writestring(" (");
            t_writestring(part_type_name(p->mbr_type));
            t_writestring(")");
        } else {
            t_writestring(part_guid_type_name(p->type_guid));
            if (p->name[0] != '\0') {
                t_writestring("  \"");
                t_writestring(p->name);
                t_putchar('"');
            }
        }

        t_writestring("  LBA=");
        t_dec(p->lba_start);
        t_writestring("  sectors=");
        t_dec(p->lba_count);
        t_writestring("  size=");
        t_dec(p->lba_count / 2048);
        t_writestring(" MiB");

        if (dp->scheme == PART_SCHEME_MBR && p->bootable)
            t_writestring("  [boot]");

        t_putchar('\n');
    }
}

/* ---------------------------------------------------------------------------
 * Built-in command handlers
 * --------------------------------------------------------------------------- */

void cmd_clear(void)
{
    /*
     * Reset both outputs and restore the Medli-compatible colour scheme
     * (white on blue).  terminal_set_colorscheme() fills the VGA buffer
     * with the new background colour; vesa_tty_setcolor() + vesa_tty_clear()
     * do the same for the VESA framebuffer.
     */
    terminal_set_colorscheme(SHELL_COLOR_VGA);
    if (vesa_tty_is_ready()) {
        vesa_tty_setcolor(SHELL_FG_RGB, SHELL_BG_RGB);
        vesa_tty_clear();
    }
}

void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            t_putchar(' ');
        t_writestring(argv[i]);
    }
    t_putchar('\n');
}

void cmd_meminfo(void)
{
    t_writestring("heap used: ");
    t_dec((uint32_t)heap_used());
    t_writestring(" bytes\n");
    t_writestring("heap free: ");
    t_dec((uint32_t)heap_free());
    t_writestring(" bytes\n");
}

void cmd_uptime(void)
{
    t_writestring("uptime: ");
    t_dec(timer_get_ticks());
    t_writestring(" ticks\n");
}

void cmd_tasks(void)
{
    static const char * const state_names[] = { "ready", "running", "dead" };
    int n = task_count();

    t_writestring("Tasks (");
    t_dec((uint32_t)n);
    t_writestring(" total):\n");

    for (int i = 0; i < n; i++) {
        task_t *t = task_get(i);
        if (!t)
            continue;
        t_writestring("  [");
        t_dec((uint32_t)i);
        t_writestring("] ");
        t_writestring(t->name ? t->name : "(unnamed)");
        t_writestring(" - ");
        t_writestring(state_names[t->state]);
        t_putchar('\n');
    }
}

void cmd_lsdisks(void)
{
    static const char * const type_str[] = { "none", "ATA", "ATAPI" };
    int found = 0;

    for (uint8_t i = 0; i < IDE_MAX_DRIVES; i++) {
        const ide_drive_t *d = ide_get_drive(i);
        if (!d || !d->present)
            continue;

        found = 1;
        t_writestring("drive ");
        t_dec(i);
        t_writestring(": [");
        t_writestring(d->channel == 0 ? "primary" : "secondary");
        t_putchar(' ');
        t_writestring(d->drive == 0 ? "master" : "slave");
        t_writestring("] ");
        t_writestring(type_str[d->type]);
        t_writestring("  ");
        t_dec(d->size / 2048);   /* MiB: sectors * 512 / (1024*1024) */
        t_writestring(" MiB  \"");
        t_writestring(d->model);
        t_writestring("\"\n");
    }

    if (!found)
        t_writestring("No drives detected.\n");
}

/* lspart <drive> — probe and display partition table (MBR or GPT). */
void cmd_lspart(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("Usage: lspart <drive>\n");
        return;
    }

    uint8_t drive = (uint8_t)parse_uint(argv[1]);

    int err = part_probe(drive, &s_disk_parts);
    if (err) {
        t_writestring("Error reading drive ");
        t_dec(drive);
        t_writestring(": ");
        t_dec((uint32_t)err);
        t_putchar('\n');
        return;
    }

    disk_parts_print(&s_disk_parts);
}

/* ---------------------------------------------------------------------------
 * mkpart helpers
 * --------------------------------------------------------------------------- */

/* Static entries array for mkpart — avoids putting ~10 KB on the stack. */
static part_info_t s_mkpart_entries[128];

/* Map a type keyword to an MBR partition type byte.  Returns 0 on failure. */
static uint8_t mkpart_mbr_type(const char *kw)
{
    if (strcmp(kw, "fat32") == 0) return PART_MBR_FAT32_LBA;
    if (strcmp(kw, "mdfs")  == 0) return PART_MBR_MDFS;
    if (strcmp(kw, "linux") == 0) return PART_MBR_LINUX;
    if (strcmp(kw, "efi")   == 0) return PART_MBR_EFI;
    return 0;
}

/* Map a type keyword to a GPT type GUID.  Returns 0 on success, -1 on error. */
static int mkpart_gpt_type(const char *kw, uint8_t *out_guid)
{
    if (strcmp(kw, "fat32") == 0) { memcpy(out_guid, PART_GUID_FAT32, 16); return 0; }
    if (strcmp(kw, "mdfs")  == 0) { memcpy(out_guid, PART_GUID_MDFS,  16); return 0; }
    if (strcmp(kw, "linux") == 0) { memcpy(out_guid, PART_GUID_LINUX, 16); return 0; }
    if (strcmp(kw, "efi")   == 0) { memcpy(out_guid, PART_GUID_EFI,   16); return 0; }
    return -1;
}

/*
 * mkpart <drive> <mbr|gpt>
 *
 * Interactive partition table creator.  Asks the user for partition count,
 * type and size; then writes MBR or GPT structure to the chosen drive.
 *
 * Supported type keywords: fat32  mdfs  linux  efi
 * Sizes are entered in MiB; they are converted to 512-byte sectors (*2048).
 *
 * MBR layout: first partition starts at LBA 2048 (1 MiB aligned).
 * GPT layout: first usable LBA is 34; partitions aligned to LBA 2048.
 */
void cmd_mkpart(int argc, char **argv)
{
    static char inbuf[64];

    if (argc < 3) {
        t_writestring("Usage: mkpart <drive> <mbr|gpt>\n");
        return;
    }

    uint8_t drive = (uint8_t)parse_uint(argv[1]);

    const ide_drive_t *drv = ide_get_drive(drive);
    if (!drv || !drv->present) {
        t_writestring("Error: drive not present.\n");
        return;
    }
    if (drv->type != IDE_TYPE_ATA) {
        t_writestring("Error: drive is not ATA.\n");
        return;
    }

    int is_gpt = (strcmp(argv[2], "gpt") == 0);
    int is_mbr = (strcmp(argv[2], "mbr") == 0);
    if (!is_gpt && !is_mbr) {
        t_writestring("Error: scheme must be 'mbr' or 'gpt'.\n");
        return;
    }

    int max_parts = is_mbr ? 4 : 128;

    /* ---------- ask partition count ---------- */
    int num_parts = 0;
    do {
        t_writestring("Number of partitions (1–");
        t_dec((uint32_t)max_parts);
        t_writestring("): ");
        shell_readline(inbuf, sizeof(inbuf));
        num_parts = (int)parse_uint(inbuf);
    } while (num_parts < 1 || num_parts > max_parts);

    /*
     * Available space for partitions.
     * MBR: first partition at LBA 2048, last usable = disk_sectors − 1.
     * GPT: first partition at LBA 2048 (first usable is 34, we align to 2048),
     *      last usable LBA = disk_sectors − 34.
     */
    uint32_t disk_sectors = drv->size;
    uint32_t next_lba     = 2048u;
    uint32_t last_lba     = is_gpt ? disk_sectors - 34u : disk_sectors - 1u;

    memset(s_mkpart_entries, 0, sizeof(s_mkpart_entries));

    for (int i = 0; i < num_parts; i++) {
        t_writestring("\nPartition ");
        t_dec((uint32_t)(i + 1));
        t_writestring(":\n");

        /* ---------- type ---------- */
        uint8_t  mbr_type = 0;
        uint8_t  gpt_guid[16];
        memset(gpt_guid, 0, 16);

        while (1) {
            t_writestring("  Type [fat32/mdfs/linux");
            if (is_gpt) t_writestring("/efi");
            t_writestring("]: ");
            shell_readline(inbuf, sizeof(inbuf));

            if (is_mbr) {
                mbr_type = mkpart_mbr_type(inbuf);
                if (mbr_type != 0) break;
            } else {
                if (mkpart_gpt_type(inbuf, gpt_guid) == 0) break;
            }
            t_writestring("  Unknown type — try again.\n");
        }

        /* ---------- size ---------- */
        uint32_t avail_mib = (last_lba - next_lba + 1u) / 2048u;
        /* Leave room for at least 1 MiB per remaining partition. */
        if (avail_mib > (uint32_t)(num_parts - i))
            avail_mib -= (uint32_t)(num_parts - i - 1);

        uint32_t size_mib = 0;
        while (size_mib < 1 || size_mib > avail_mib) {
            t_writestring("  Size in MiB (1–");
            t_dec(avail_mib);
            t_writestring("): ");
            shell_readline(inbuf, sizeof(inbuf));
            size_mib = parse_uint(inbuf);
        }

        uint32_t lba_count = size_mib * 2048u;

        /* ---------- optional name (GPT only) ---------- */
        if (is_gpt) {
            t_writestring("  Partition name (Enter for none): ");
            shell_readline(inbuf, sizeof(inbuf));
        }

        /* ---------- fill in descriptor ---------- */
        part_info_t *p = &s_mkpart_entries[i];
        p->scheme    = is_gpt ? PART_SCHEME_GPT : PART_SCHEME_MBR;
        p->mbr_type  = mbr_type;
        p->bootable  = 0;
        p->lba_start = next_lba;
        p->lba_count = lba_count;
        if (is_gpt) {
            memcpy(p->type_guid, gpt_guid, 16);
            /* part_guid left as zero; part_write_gpt() will generate one. */
            /* Copy at most 36 characters of the partition name. */
            for (int k = 0; k < 36 && inbuf[k] != '\0'; k++)
                p->name[k] = inbuf[k];
            p->name[36] = '\0';
        }

        next_lba += lba_count;
        /* Align the start of the next partition to a 2048-sector boundary. */
        if (next_lba % 2048u)
            next_lba = (next_lba / 2048u + 1u) * 2048u;
    }

    /* ---------- write ---------- */
    t_writestring("\nWriting ");
    t_writestring(is_gpt ? "GPT" : "MBR");
    t_writestring(" to drive ");
    t_dec(drive);
    t_writestring("...\n");

    int err;
    if (is_mbr)
        err = part_write_mbr(drive, s_mkpart_entries, num_parts);
    else
        err = part_write_gpt(drive, s_mkpart_entries, num_parts);

    if (err) {
        t_writestring("Error writing partition table: ");
        t_dec((uint32_t)err);
        t_putchar('\n');
        return;
    }

    t_writestring("Done.\n\n");

    /* Verify by re-reading and displaying the result. */
    err = part_probe(drive, &s_disk_parts);
    if (err == 0)
        disk_parts_print(&s_disk_parts);
}

static uint8_t sector_buf[512];

void cmd_readsector(int argc, char **argv)
{
    if (argc < 3) {
        t_writestring("Usage: readsector <drive> <lba>\n");
        return;
    }

    uint8_t  drive = (uint8_t)parse_uint(argv[1]);
    uint32_t lba   = parse_uint(argv[2]);

    const ide_drive_t *d = ide_get_drive(drive);
    if (!d || !d->present) {
        t_writestring("Error: drive not present.\n");
        return;
    }
    if (d->type != IDE_TYPE_ATA) {
        t_writestring("Error: drive is not ATA (read not supported).\n");
        return;
    }

    int err = ide_read_sectors(drive, lba, 1, sector_buf);
    if (err) {
        t_writestring("Read error: ");
        t_dec((uint32_t)err);
        t_putchar('\n');
        return;
    }

    t_writestring("Sector ");
    t_dec(lba);
    t_writestring(" of drive ");
    t_dec(drive);
    t_writestring(":\n");
    hexdump_sector(sector_buf);
}

void cmd_setmode(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("Usage: setmode <25|50>\n");
        return;
    }

    uint32_t rows = parse_uint(argv[1]);
    if (rows != 25 && rows != 50) {
        t_writestring("Error: only 25 and 50 are supported.\n");
        return;
    }

    /*
     * VGA text mode: reprogram CRTC Max Scan Line register so the
     * character cell is 16 rows tall (80x25) or 8 rows tall (80x50),
     * then reinitialise the terminal to clear and reset the cursor.
     *
     * VESA framebuffer: change the font scale factor so glyphs are
     * rendered at 16x16 px (scale=2, ~25 lines) or 8x8 px (scale=1,
     * ~50 lines), then clear the framebuffer.
     */
    uint32_t vesa_scale = (rows == 50) ? 1 : 2;
    terminal_set_rows((size_t)rows);
    vesa_tty_set_scale(vesa_scale);
}

void cmd_shutdown(void)
{
    acpi_shutdown(); /* never returns */
}

void cmd_reboot(void)
{
    acpi_reboot(); /* never returns */
}

/* ---------------------------------------------------------------------------
 * FAT32 filesystem commands
 * --------------------------------------------------------------------------- */

/* Shared static partition table used by mount/mkfs (avoids stack pressure). */
static disk_parts_t s_cmd_parts;

/*
 * mount <drive> <part>
 *
 * Probe the partition table on <drive> and mount partition number <part>
 * (1-based, matching lspart output) as a FAT32 volume accessible at /hd/.
 */
void cmd_mount(int argc, char **argv)
{
    if (argc < 3) {
        t_writestring("Usage: mount <drive> <part>\n");
        return;
    }

    uint8_t drive    = (uint8_t)parse_uint(argv[1]);
    int     part_num = (int)parse_uint(argv[2]);  /* 1-based, matches lspart */

    int err = part_probe(drive, &s_cmd_parts);
    if (err) {
        t_writestring("mount: drive not accessible\n");
        return;
    }

    int part_idx = part_num - 1;   /* convert to 0-based array index */
    if (part_idx < 0 || part_idx >= s_cmd_parts.count) {
        t_writestring("mount: invalid partition number");
        if (s_cmd_parts.count > 0) {
            t_writestring(" (valid: 1–");
            t_dec((uint32_t)s_cmd_parts.count);
            t_putchar(')');
        }
        t_writestring("\n       (use lspart ");
        t_dec(drive);
        t_writestring(" to list partitions)\n");
        return;
    }

    uint32_t lba = s_cmd_parts.parts[part_idx].lba_start;

    err = fat32_mount(drive, lba);
    if (err) {
        t_writestring("mount: not a valid FAT32 volume (error ");
        t_dec((uint32_t)(-err));
        t_writestring(")\n");
        return;
    }

    vfs_notify_hd_mounted();

    t_writestring("Mounted FAT32  drive ");
    t_dec(drive);
    t_writestring("  partition ");
    t_dec((uint32_t)part_num);
    t_writestring("  LBA ");
    t_dec(lba);
    t_writestring("\ncwd: ");
    t_writestring(vfs_getcwd());
    t_putchar('\n');
}

/* umount — unmount the current FAT32 volume. */
void cmd_umount(void)
{
    if (!fat32_mounted()) {
        t_writestring("umount: no volume mounted\n");
        return;
    }
    fat32_unmount();
    vfs_notify_hd_unmounted();
    t_writestring("Volume unmounted.\n");
}

/* ls [path] — list directory via the VFS (supports /hd/… and /cdrom/…). */
void cmd_ls(int argc, char **argv)
{
    const char *path = (argc >= 2) ? argv[1] : NULL;

    t_writestring(path ? path : vfs_getcwd());
    t_writestring(":\n");

    vfs_ls(path);
}

/* cat <file> — read and display a file via the VFS (supports /hd/… and /cdrom/…). */
void cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("Usage: cat <file>\n");
        return;
    }
    vfs_cat(argv[1]);
}

/* cd <path> — change working directory via the VFS. */
void cmd_cd(int argc, char **argv)
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

/* mkdir <path> — create a directory via the VFS (FAT32 /hd/ only). */
void cmd_mkdir(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("Usage: mkdir <path>\n");
        return;
    }

    int err = vfs_mkdir(argv[1]);
    switch (err) {
    case  0: t_writestring("Directory created.\n");          break;
    case -1: t_writestring("mkdir: path error\n");           break;
    case -2: t_writestring("mkdir: I/O error\n");            break;
    case -4: t_writestring("mkdir: disk full\n");            break;
    case -6: t_writestring("mkdir: already exists\n");       break;
    default:
        if (err < 0) {
            t_writestring("mkdir: error ");
            t_dec((uint32_t)(-err));
            t_putchar('\n');
        }
        break;
    }
}

/*
 * mkfs <drive> <part>
 *
 * Format partition <part> on <drive> as FAT32.
 * <part> is 1-based to match lspart output (use mkpart first).
 */
void cmd_mkfs(int argc, char **argv)
{
    if (argc < 3) {
        t_writestring("Usage: mkfs <drive> <part>\n");
        return;
    }

    uint8_t drive    = (uint8_t)parse_uint(argv[1]);
    int     part_num = (int)parse_uint(argv[2]);  /* 1-based, matches lspart */

    int err = part_probe(drive, &s_cmd_parts);
    if (err) {
        t_writestring("mkfs: drive not accessible\n");
        return;
    }

    int part_idx = part_num - 1;   /* convert to 0-based array index */
    if (part_idx < 0 || part_idx >= s_cmd_parts.count) {
        t_writestring("mkfs: invalid partition number");
        if (s_cmd_parts.count > 0) {
            t_writestring(" (valid: 1–");
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

/* ---------------------------------------------------------------------------
 * ISO9660 commands
 * --------------------------------------------------------------------------- */

/* isols <drive> [path] — list a directory on an ISO9660 CD-ROM. */
void cmd_isols(int argc, char **argv)
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

/* ---------------------------------------------------------------------------
 * Installer command
 * --------------------------------------------------------------------------- */

void cmd_install(void)
{
    installer_run();
}

/* ---------------------------------------------------------------------------
 * vics — interactive text editor
 * --------------------------------------------------------------------------- */

void cmd_vics(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("Usage: vics <filename>\n");
        t_writestring("  Opens <filename> for editing. Creates the file on save if it does not exist.\n");
        t_writestring("  Key bindings: Arrow keys navigate | Ctrl+S save | Ctrl+Q quit\n");
        return;
    }

    /* Resolve a bare filename against the current working directory. */
    const char *arg  = argv[1];
    const char *cwd  = vfs_getcwd();

    static char full_path[VFS_PATH_MAX];

    if (arg[0] == '/') {
        /* Absolute path — use as-is. */
        strncpy(full_path, arg, VFS_PATH_MAX - 1);
        full_path[VFS_PATH_MAX - 1] = '\0';
    } else {
        /* Relative path — prepend CWD. */
        size_t cwd_len = strlen(cwd);
        size_t arg_len = strlen(arg);
        if (cwd_len + 1 + arg_len >= VFS_PATH_MAX) {
            t_writestring("vics: path too long\n");
            return;
        }
        size_t off = 0;
        memcpy(full_path, cwd, cwd_len); off += cwd_len;
        if (cwd[cwd_len - 1] != '/')
            full_path[off++] = '/';
        memcpy(full_path + off, arg, arg_len + 1);
    }

    vics_edit(full_path);
}

/* ---------------------------------------------------------------------------
 * eject — unmount and eject a storage device.
 *
 * Usage:
 *   eject hdd    — flush and unmount the FAT32 volume mounted at /hd
 *   eject cdrom  — unmount /cdrom and send the ATAPI eject command so the
 *                  tray opens (works in QEMU and on real drives with a
 *                  software-controllable tray)
 * --------------------------------------------------------------------------- */

void cmd_eject(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("Usage: eject hdd|cdrom\n");
        return;
    }

    const char *target = argv[1];

    if (strcmp(target, "hdd") == 0) {
        if (!fat32_mounted()) {
            t_writestring("eject: no HDD volume is mounted\n");
            return;
        }
        fat32_unmount();
        vfs_notify_hd_unmounted();
        t_writestring("HDD volume unmounted.\n");
        return;
    }

    if (strcmp(target, "cdrom") == 0) {
        /* Find the ATAPI drive that was registered by vfs_init(). */
        int cd_drive = -1;
        for (int i = 0; i < IDE_MAX_DRIVES; i++) {
            const ide_drive_t *d = ide_get_drive((uint8_t)i);
            if (d && d->present && d->type == IDE_TYPE_ATAPI) {
                cd_drive = i;
                break;
            }
        }

        if (cd_drive < 0) {
            t_writestring("eject: no CD-ROM drive detected\n");
            return;
        }

        /* Deregister /cdrom in the VFS before sending the hardware command. */
        vfs_notify_cdrom_ejected();

        int err = ide_eject_atapi((uint8_t)cd_drive);
        if (err) {
            t_writestring("eject: ATAPI eject command failed (err ");
            t_dec((uint32_t)err);
            t_writestring(")\n");
        } else {
            t_writestring("CD-ROM ejected.\n");
        }
        return;
    }

    t_writestring("eject: unknown target '");
    t_writestring(target);
    t_writestring("' — use 'hdd' or 'cdrom'\n");
}
