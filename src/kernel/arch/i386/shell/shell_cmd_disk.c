/*
 * shell_cmd_disk.c -- disk and partition shell commands.
 *
 * Commands: lsdisks  lspart  mkpart  readsector  chainload
 */

#include "shell_priv.h"

#include <kernel/tty.h>
#include <kernel/ide.h>
#include <kernel/partition.h>
#include <kernel/chainload.h>

/* ---------------------------------------------------------------------------
 * Private helpers
 * --------------------------------------------------------------------------- */

static void hexdump_sector(const uint8_t *buf)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int row = 0; row < 32; row++) {
        int offset = row * 16;
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

static disk_parts_t s_disk_parts;

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
 * Command handlers
 * --------------------------------------------------------------------------- */

static void cmd_lsdisks(int argc, char **argv)
{
    (void)argc; (void)argv;
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
        t_dec(d->size / 2048);
        t_writestring(" MiB  \"");
        t_writestring(d->model);
        t_writestring("\"\n");
    }

    if (!found)
        t_writestring("No drives detected.\n");
}

static void cmd_lspart(int argc, char **argv)
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

/* Static entries array for mkpart - avoids putting ~10 KB on the stack. */
static part_info_t s_mkpart_entries[128];

static uint8_t mkpart_mbr_type(const char *kw)
{
    if (strcmp(kw, "fat32") == 0) return PART_MBR_FAT32_LBA;
    if (strcmp(kw, "mdfs")  == 0) return PART_MBR_MDFS;
    if (strcmp(kw, "linux") == 0) return PART_MBR_LINUX;
    if (strcmp(kw, "efi")   == 0) return PART_MBR_EFI;
    return 0;
}

static int mkpart_gpt_type(const char *kw, uint8_t *out_guid)
{
    if (strcmp(kw, "fat32") == 0) { memcpy(out_guid, PART_GUID_FAT32, 16); return 0; }
    if (strcmp(kw, "mdfs")  == 0) { memcpy(out_guid, PART_GUID_MDFS,  16); return 0; }
    if (strcmp(kw, "linux") == 0) { memcpy(out_guid, PART_GUID_LINUX, 16); return 0; }
    if (strcmp(kw, "efi")   == 0) { memcpy(out_guid, PART_GUID_EFI,   16); return 0; }
    return -1;
}

static void cmd_mkpart(int argc, char **argv)
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

    int num_parts = 0;
    do {
        t_writestring("Number of partitions (1–");
        t_dec((uint32_t)max_parts);
        t_writestring("): ");
        shell_readline(inbuf, sizeof(inbuf));
        num_parts = (int)parse_uint(inbuf);
    } while (num_parts < 1 || num_parts > max_parts);

    uint32_t disk_sectors = drv->size;
    uint32_t next_lba     = 2048u;
    uint32_t last_lba     = is_gpt ? disk_sectors - 34u : disk_sectors - 1u;

    memset(s_mkpart_entries, 0, sizeof(s_mkpart_entries));

    for (int i = 0; i < num_parts; i++) {
        t_writestring("\nPartition ");
        t_dec((uint32_t)(i + 1));
        t_writestring(":\n");

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
            t_writestring("  Unknown type - try again.\n");
        }

        uint32_t avail_mib = (last_lba - next_lba + 1u) / 2048u;
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

        if (is_gpt) {
            t_writestring("  Partition name (Enter for none): ");
            shell_readline(inbuf, sizeof(inbuf));
        }

        part_info_t *p = &s_mkpart_entries[i];
        p->scheme    = is_gpt ? PART_SCHEME_GPT : PART_SCHEME_MBR;
        p->mbr_type  = mbr_type;
        p->bootable  = 0;
        p->lba_start = next_lba;
        p->lba_count = lba_count;
        if (is_gpt) {
            memcpy(p->type_guid, gpt_guid, 16);
            for (int k = 0; k < 36 && inbuf[k] != '\0'; k++)
                p->name[k] = inbuf[k];
            p->name[36] = '\0';
        }

        next_lba += lba_count;
        if (next_lba % 2048u)
            next_lba = (next_lba / 2048u + 1u) * 2048u;
    }

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

    err = part_probe(drive, &s_disk_parts);
    if (err == 0)
        disk_parts_print(&s_disk_parts);
}

static uint8_t sector_buf[512];

static void cmd_readsector(int argc, char **argv)
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

/* 512-byte sector staging buffer for chainload. */
static uint8_t s_chainload_sector[512];

static void cmd_chainload(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("Usage: chainload <drive> [lba]\n");
        t_writestring("  lba defaults to 0 (MBR / bootloader sector)\n");
        return;
    }

    uint8_t  drive = (uint8_t)parse_uint(argv[1]);
    uint32_t lba   = (argc >= 3) ? parse_uint(argv[2]) : 0;

    const ide_drive_t *drv = ide_get_drive(drive);
    if (!drv || !drv->present) {
        t_writestring("chainload: drive not found\n");
        return;
    }

    if (ide_read_sectors(drive, lba, 1, s_chainload_sector) != 0) {
        t_writestring("chainload: read error\n");
        return;
    }

    if (s_chainload_sector[510] != 0x55 || s_chainload_sector[511] != 0xAA) {
        t_writestring("chainload: sector has no boot signature (0x55AA)\n");
        return;
    }

    uint8_t *dest = (uint8_t *)0x7C00;
    for (int i = 0; i < 512; i++)
        dest[i] = s_chainload_sector[i];

    t_writestring("chainload: handing off to drive 0x");
    t_hex(0x80 + drive);
    t_writestring(" LBA ");
    t_dec(lba);
    t_writestring("\n");

    chainload_enter((uint8_t)(0x80 + drive)); /* never returns */
}

/* ---------------------------------------------------------------------------
 * Module table
 * --------------------------------------------------------------------------- */

const shell_cmd_entry_t disk_cmds[] = {
    { "lsdisks",    cmd_lsdisks    },
    { "lspart",     cmd_lspart     },
    { "mkpart",     cmd_mkpart     },
    { "readsector", cmd_readsector },
    { "chainload",  cmd_chainload  },
    { NULL, NULL }
};
