/*
 * installer.c - interactive OS-to-disk installer.
 *
 * Implements the full installation sequence documented in installer.h.
 * All sector-level buffers are declared at file scope to avoid large
 * kernel-stack allocations.
 */

#include <kernel/installer.h>
#include <kernel/ide.h>
#include <kernel/iso9660.h>
#include <kernel/partition.h>
#include <kernel/fat32.h>
#include <kernel/vfs.h>
#include <kernel/heap.h>
#include <kernel/tty.h>
#include <kernel/keyboard.h>
#include <string.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Tunables
 * ---------------------------------------------------------------------- */

/* First partition starts at LBA 2048 (1 MiB aligned, GRUB-safe). */
#define INST_PART_START_LBA    2048u

/*
 * Maximum file size the installer will copy in a single operation (8 MiB).
 * The kernel is typically 1–3 MiB; GRUB modules are generally < 512 KiB.
 */
#define INST_MAX_FILE_SIZE     (8u * 1024u * 1024u)

/* Offset within boot.img of the 64-bit LE LBA address of core.img. */
#define BOOT_IMG_CORE_LBA_OFF  0x5Cu

/* Byte offset of the four 16-byte MBR partition entries. */
#define MBR_PART_TABLE_OFF     0x1BEu

/* Byte offset of the MBR boot signature (0x55, 0xAA). */
#define MBR_SIG_OFF            0x1FEu

/* -------------------------------------------------------------------------
 * File-scope buffers - never on the kernel stack
 * ---------------------------------------------------------------------- */
static uint8_t s_mbr[512];        /* scratch sector for the MBR           */
static uint8_t s_boot_img[512];   /* boot.img read from the ISO           */

/* -------------------------------------------------------------------------
 * inst_readline – read a line from the PS/2 keyboard into buf[0..max-1].
 * Echoes characters, handles backspace, terminates on Enter.
 * ---------------------------------------------------------------------- */
static void inst_readline(char *buf, size_t max)
{
    size_t len = 0;

    while (1) {
        char c = keyboard_getchar();

        if (c == '\n' || c == '\r') {
            t_putchar('\n');
            break;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                t_backspace();
            }
            continue;
        }
        if (c < 0x20 || c > 0x7E)
            continue;
        if (len < max - 1) {
            buf[len++] = c;
            t_putchar(c);
        }
    }

    buf[len] = '\0';
}

/* -------------------------------------------------------------------------
 * inst_wr32 – write a 32-bit value in little-endian byte order.
 * ---------------------------------------------------------------------- */
static void inst_wr32(uint8_t *b, int off, uint32_t v)
{
    b[off]     = (uint8_t)(v);
    b[off + 1] = (uint8_t)(v >>  8);
    b[off + 2] = (uint8_t)(v >> 16);
    b[off + 3] = (uint8_t)(v >> 24);
}

/* -------------------------------------------------------------------------
 * copy_file – copy one file from ISO9660 to the mounted FAT32 volume.
 *
 * cd_drive : IDE drive index of the ATAPI CD-ROM
 * src_path : absolute path on the ISO  (e.g. "/boot/makar.kernel")
 * dst_path : absolute path on the FAT32 (e.g. "/boot/makar.kernel")
 * file_buf : caller-allocated buffer of at least INST_MAX_FILE_SIZE bytes
 *
 * Returns  0 on success.
 * Returns -1 on ISO read error.
 * Returns -2 on FAT32 write error.
 * ---------------------------------------------------------------------- */
static int copy_file(uint8_t cd_drive,
                     const char *src_path, const char *dst_path,
                     uint8_t *file_buf)
{
    t_writestring("  ");
    t_writestring(src_path);
    t_writestring(" -> ");
    t_writestring(dst_path);
    t_writestring(" ... ");

    uint32_t sz = 0;
    int err = iso9660_read_file(cd_drive, src_path,
                                file_buf, INST_MAX_FILE_SIZE, &sz);
    if (err) {
        t_writestring("read error\n");
        return -1;
    }

    err = fat32_write_file(dst_path, file_buf, sz);
    if (err) {
        t_writestring("write error\n");
        return -2;
    }

    t_dec(sz);
    t_writestring(" B\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * installer_run
 * ---------------------------------------------------------------------- */
void installer_run(void)
{
    static char inbuf[16];

    t_writestring("\n=== Makar OS Installer ===\n\n");

    /* ------------------------------------------------------------------
     * Step 1: Find the source ISO9660 CD-ROM.
     * ------------------------------------------------------------------ */
    t_writestring("Scanning for ISO9660 CD-ROM source...\n");

    int cd_drive = -1;
    for (int i = 0; i < IDE_MAX_DRIVES; i++) {
        const ide_drive_t *d = ide_get_drive((uint8_t)i);
        if (!d || !d->present || d->type != IDE_TYPE_ATAPI)
            continue;
        if (iso9660_probe((uint8_t)i) == 0) {
            cd_drive = i;
            t_writestring("  found ISO9660 on drive ");
            t_dec((uint32_t)i);
            t_writestring(" (");
            t_writestring(d->model);
            t_writestring(")\n");
            break;
        }
    }

    if (cd_drive < 0) {
        t_writestring("Error: no ISO9660 CD-ROM found.\n");
        return;
    }

    /* ------------------------------------------------------------------
     * Step 2: List ATA target drives and prompt for selection.
     * ------------------------------------------------------------------ */
    t_writestring("\nAvailable ATA target drives:\n");

    int ata_count = 0;
    for (int i = 0; i < IDE_MAX_DRIVES; i++) {
        const ide_drive_t *d = ide_get_drive((uint8_t)i);
        if (!d || !d->present || d->type != IDE_TYPE_ATA)
            continue;
        t_writestring("  drive ");
        t_dec((uint32_t)i);
        t_writestring(": ");
        t_dec(d->size / 2048u);
        t_writestring(" MiB  \"");
        t_writestring(d->model);
        t_writestring("\"\n");
        ata_count++;
    }

    if (ata_count == 0) {
        t_writestring("Error: no ATA target drives found.\n");
        return;
    }

    int hdd_drive = -1;
    while (hdd_drive < 0) {
        t_writestring("Enter target drive number: ");
        inst_readline(inbuf, sizeof(inbuf));

        int n = 0;
        const char *cp = inbuf;
        while (*cp >= '0' && *cp <= '9')
            n = n * 10 + (*cp++ - '0');

        if (n >= 0 && n < IDE_MAX_DRIVES) {
            const ide_drive_t *d = ide_get_drive((uint8_t)n);
            if (d && d->present && d->type == IDE_TYPE_ATA)
                hdd_drive = n;
        }
        if (hdd_drive < 0)
            t_writestring("  Invalid selection - try again.\n");
    }

    const ide_drive_t *hdd = ide_get_drive((uint8_t)hdd_drive);

    /* ------------------------------------------------------------------
     * Step 3: Confirm destructive write.
     * ------------------------------------------------------------------ */
    t_writestring("\nWARNING: ALL DATA on drive ");
    t_dec((uint32_t)hdd_drive);
    t_writestring(" (\"");
    t_writestring(hdd->model);
    t_writestring("\", ");
    t_dec(hdd->size / 2048u);
    t_writestring(" MiB) will be DESTROYED.\n");
    t_writestring("Type 'yes' to continue: ");
    inst_readline(inbuf, sizeof(inbuf));

    if (strcmp(inbuf, "yes") != 0) {
        t_writestring("Installation cancelled.\n");
        return;
    }

    /* Allocate the shared file-copy buffer once, reuse for all transfers. */
    uint8_t *file_buf = (uint8_t *)kmalloc(INST_MAX_FILE_SIZE);
    if (!file_buf) {
        t_writestring("Error: out of memory for transfer buffer.\n");
        return;
    }

    /* ------------------------------------------------------------------
     * Steps 4–5: Read boot.img and core.img from ISO; install GRUB.
     * ------------------------------------------------------------------ */
    t_writestring("\nInstalling GRUB bootloader...\n");

    /* Read boot.img (512 bytes) from the ISO. */
    uint32_t boot_sz = 0;
    if (iso9660_read_file((uint8_t)cd_drive,
                          "/boot/grub/i386-pc/boot.img",
                          s_boot_img, sizeof(s_boot_img), &boot_sz) != 0
        || boot_sz < 446u)
    {
        t_writestring("Error: failed to read /boot/grub/i386-pc/boot.img.\n");
        kfree(file_buf);
        return;
    }

    /* Read core.img from the ISO into the shared buffer. */
    uint32_t core_sz = 0;
    if (iso9660_read_file((uint8_t)cd_drive,
                          "/boot/grub/i386-pc/core.img",
                          file_buf, INST_MAX_FILE_SIZE, &core_sz) != 0
        || core_sz == 0)
    {
        t_writestring("Error: failed to read /boot/grub/i386-pc/core.img.\n");
        kfree(file_buf);
        return;
    }

    /*
     * Patch boot.img: set the 64-bit LE LBA of core.img to 1
     * (core.img will live at sector 1 of the target HDD).
     */
    inst_wr32(s_boot_img, BOOT_IMG_CORE_LBA_OFF,     1u);  /* low  32 bits */
    inst_wr32(s_boot_img, BOOT_IMG_CORE_LBA_OFF + 4,  0u); /* high 32 bits */

    /*
     * Build the 512-byte MBR:
     *   bytes   0–445 : GRUB boot code from boot.img
     *   bytes 446–509 : partition table (one FAT32 LBA entry, bootable)
     *   bytes 510–511 : boot signature 0x55AA
     */
    memset(s_mbr, 0, sizeof(s_mbr));
    memcpy(s_mbr, s_boot_img, 446u);

    uint32_t disk_sectors = hdd->size;
    uint32_t part_start   = INST_PART_START_LBA;
    uint32_t part_count   = disk_sectors - part_start;

    /* First (and only) partition entry at byte 0x1BE. */
    uint8_t *pe = s_mbr + MBR_PART_TABLE_OFF;
    pe[0] = 0x80u;              /* bootable flag                           */
    pe[1] = 0xFEu;              /* CHS start - beyond range, use LBA       */
    pe[2] = 0xFFu;
    pe[3] = 0xFFu;
    pe[4] = PART_MBR_FAT32_LBA; /* partition type 0x0C                    */
    pe[5] = 0xFEu;              /* CHS end - beyond range                  */
    pe[6] = 0xFFu;
    pe[7] = 0xFFu;
    inst_wr32(pe,  8, part_start);
    inst_wr32(pe, 12, part_count);

    /* Boot signature. */
    s_mbr[MBR_SIG_OFF]     = 0x55u;
    s_mbr[MBR_SIG_OFF + 1] = 0xAAu;

    /* Write the MBR to sector 0 of the target HDD. */
    if (ide_write_sectors((uint8_t)hdd_drive, 0u, 1u, s_mbr)) {
        t_writestring("Error: failed to write MBR to target drive.\n");
        kfree(file_buf);
        return;
    }

    /*
     * Write core.img to sectors 1..N (the GRUB embedding area between the
     * MBR and the first partition at LBA 2048).
     * Round up to a 512-byte sector boundary; zero-pad the last sector.
     */
    uint32_t core_sectors = (core_sz + 511u) / 512u;

    if (core_sectors + 1u > INST_PART_START_LBA) {
        t_writestring("Error: core.img too large for the embedding area.\n");
        kfree(file_buf);
        return;
    }

    /* Zero-pad the buffer to a full sector boundary. */
    uint32_t core_padded = core_sectors * 512u;
    if (core_padded > core_sz)
        memset(file_buf + core_sz, 0, core_padded - core_sz);

    /*
     * ide_write_sectors() takes a uint8_t sector count (max 255 per call).
     * Write core.img in batches of up to 127 sectors.
     */
    uint32_t written = 0;
    while (written < core_sectors) {
        uint32_t batch = core_sectors - written;
        if (batch > 127u)
            batch = 127u;
        if (ide_write_sectors((uint8_t)hdd_drive,
                              1u + written, (uint8_t)batch,
                              file_buf + written * 512u)) {
            t_writestring("Error: failed to write core.img sectors.\n");
            kfree(file_buf);
            return;
        }
        written += batch;
    }

    t_writestring("  boot.img written to MBR, core.img written to sectors 1–");
    t_dec(core_sectors);
    t_writestring("\n");

    /* ------------------------------------------------------------------
     * Step 6: Format the partition as FAT32.
     * ------------------------------------------------------------------ */
    t_writestring("Formatting FAT32 partition...\n");

    if (fat32_mkfs((uint8_t)hdd_drive, part_start, part_count) != 0) {
        t_writestring("Error: FAT32 format failed.\n");
        kfree(file_buf);
        return;
    }
    t_writestring("  done.\n");

    /* ------------------------------------------------------------------
     * Step 7: Mount the FAT32 volume.
     * ------------------------------------------------------------------ */
    if (fat32_mounted())
        fat32_unmount();

    if (fat32_mount((uint8_t)hdd_drive, part_start) != 0) {
        t_writestring("Error: failed to mount the new FAT32 volume.\n");
        kfree(file_buf);
        return;
    }

    /* ------------------------------------------------------------------
     * Step 8: Create directory tree.
     * ------------------------------------------------------------------ */
    t_writestring("Creating directory tree...\n");
    fat32_mkdir("/boot");
    fat32_mkdir("/boot/grub");
    fat32_mkdir("/boot/grub/i386-pc");

    /* ------------------------------------------------------------------
     * Step 9: Copy the kernel.
     * ------------------------------------------------------------------ */
    t_writestring("Copying kernel...\n");
    if (copy_file((uint8_t)cd_drive,
                  "/boot/makar.kernel", "/boot/makar.kernel",
                  file_buf) != 0) {
        t_writestring("Warning: kernel copy failed - installation may be incomplete.\n");
    }

    /* ------------------------------------------------------------------
     * Step 10: Copy essential GRUB modules.
     * ------------------------------------------------------------------ */
    t_writestring("Copying GRUB modules...\n");

    static const char * const modules[] = {
        "normal.mod",
        "part_msdos.mod",
        "fat.mod",
        "multiboot2.mod",
        "linux.mod",
        NULL
    };

    static const char grub_mod_dir[] = "/boot/grub/i386-pc/";
    size_t pflen = sizeof(grub_mod_dir) - 1u;  /* length without NUL */

    for (int i = 0; modules[i] != NULL; i++) {
        char src[128];
        char dst[128];
        size_t mlen = strlen(modules[i]);

        if (pflen + mlen >= sizeof(src))
            continue;

        memcpy(src, grub_mod_dir, pflen);
        memcpy(src + pflen, modules[i], mlen + 1u);
        memcpy(dst, grub_mod_dir, pflen);
        memcpy(dst + pflen, modules[i], mlen + 1u);

        /* A missing module is not fatal - skip silently on error. */
        copy_file((uint8_t)cd_drive, src, dst, file_buf);
    }

    /* ------------------------------------------------------------------
     * Step 11: Write /boot/grub/grub.cfg.
     * ------------------------------------------------------------------ */
    t_writestring("Writing grub.cfg...\n");

    /*
     * HDD grub.cfg differs from the CD grub.cfg in two key ways:
     *   1. set timeout=0  - boot immediately with no user interaction required.
     *   2. Explicit 'boot' command inside the menuentry so the kernel loads
     *      correctly even when GRUB is invoked manually from its command line.
     * The CD grub.cfg uses the same multiboot2 path because the ISO9660 and
     * the FAT32 partition share the same /boot/makar.kernel layout.
     */
    static const char grub_cfg[] =
        "set default=0\n"
        "set timeout=0\n"
        "\n"
        "menuentry \"Makar OS\" {\n"
        "\tmultiboot2 /boot/makar.kernel\n"
        "\tboot\n"
        "}\n";

    if (fat32_write_file("/boot/grub/grub.cfg",
                         grub_cfg, (uint32_t)(sizeof(grub_cfg) - 1u)) != 0) {
        t_writestring("Warning: failed to write grub.cfg.\n");
    }

    /* ------------------------------------------------------------------
     * Step 12: Unmount and report success.
     * ------------------------------------------------------------------ */
    fat32_unmount();
    vfs_notify_hd_unmounted();
    kfree(file_buf);

    t_writestring("\n=== Installation complete! ===\n");
    t_writestring("Remove the CD-ROM and reboot to start Makar from the hard drive.\n\n");
}
