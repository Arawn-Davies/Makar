/*
 * fdisk.elf - MBR partition editor for Makar.
 *
 * Opens a /dev block device, reads the 512-byte MBR at LBA 0, and lets
 * the operator inspect and edit the four primary partition entries.
 * Mirrors the classic util-linux fdisk command set (p/n/d/t/a/w/q) but
 * MBR-only and primary-partitions-only.
 *
 *   fdisk            edit /dev/hda
 *   fdisk /dev/hdb   edit a specific disk
 *
 * Writes go straight back to LBA 0 via SYS_WRITE on the open fd; the
 * kernel's devfs does the read-modify-write so the bootstrap code in
 * bytes 0x000-0x1BD is preserved.
 */

#include "syscall.h"

#define SECTOR_SIZE   512
#define PART_OFFSET   0x1BE      /* first partition entry                */
#define PART_ENTRY    16         /* bytes per entry                      */
#define NUM_PARTS     4

static unsigned char mbr[SECTOR_SIZE];
static char          line[128];

/* ---- tiny stdio ------------------------------------------------------- */

static unsigned int ustrlen(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

static void puts_(const char *s) { sys_write(1, s, ustrlen(s)); }

static void putu(unsigned int v)
{
    char t[12];
    int n = 0;
    if (v == 0) { sys_write(1, "0", 1); return; }
    while (v && n < 12) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    char o[12];
    for (int i = 0; i < n; i++) o[i] = t[n - 1 - i];
    sys_write(1, o, (unsigned int)n);
}

static void puthex2(unsigned char v)
{
    const char *h = "0123456789ABCDEF";
    char o[2] = { h[(v >> 4) & 0xF], h[v & 0xF] };
    sys_write(1, o, 2);
}

/* Read one line from stdin (without trailing newline).  Returns length. */
static int readline(void)
{
    long n = sys_read(0, line, (unsigned int)sizeof(line) - 1);
    if (n <= 0) { line[0] = '\0'; return 0; }
    int len = (int)n;
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    line[len] = '\0';
    return len;
}

static unsigned int parse_uint(const char *s)
{
    unsigned int v = 0;
    while (*s == ' ') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (*s) {
            char c = *s++;
            unsigned int d;
            if (c >= '0' && c <= '9') d = (unsigned int)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (unsigned int)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (unsigned int)(c - 'A' + 10);
            else break;
            v = v * 16 + d;
        }
        return v;
    }
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned int)(*s++ - '0');
    return v;
}

/* ---- MBR entry access (little-endian) --------------------------------- */

static unsigned char *entry(int i) { return &mbr[PART_OFFSET + i * PART_ENTRY]; }

static unsigned int le32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void wr32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v);       p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static const char *type_name(unsigned char t)
{
    switch (t) {
    case 0x00: return "Empty";
    case 0x01: return "FAT12";
    case 0x04: return "FAT16 <32M";
    case 0x05: return "Extended";
    case 0x06: return "FAT16";
    case 0x07: return "NTFS/exFAT";
    case 0x0B: return "FAT32 CHS";
    case 0x0C: return "FAT32 LBA";
    case 0x0E: return "FAT16 LBA";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI System";
    case 0xFA: return "MDFS";
    default:   return "Unknown";
    }
}

static void print_table(const char *dev)
{
    puts_("Disk ");
    puts_(dev);
    puts_("\n  #  Boot   Start      Sectors    Size(MiB)  Id  Type\n");
    for (int i = 0; i < NUM_PARTS; i++) {
        unsigned char *e = entry(i);
        unsigned char boot = e[0];
        unsigned char id   = e[4];
        unsigned int start = le32(e + 8);
        unsigned int count = le32(e + 12);

        puts_("  ");
        putu((unsigned int)(i + 1));
        puts_("  ");
        puts_(boot == 0x80 ? " *  " : "    ");
        puts_(" ");
        putu(start);
        puts_("\t");
        putu(count);
        puts_("\t");
        putu(count / 2048u);          /* 512-byte sectors -> MiB */
        puts_("\t   ");
        puthex2(id);
        puts_("  ");
        puts_(type_name(id));
        puts_("\n");
    }
}

static int valid_index(unsigned int n) { return n >= 1 && n <= NUM_PARTS; }

int main(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : "/dev/hda";

    int fd = sys_open(dev, O_RDWR);
    if (fd < 0) {
        puts_("fdisk: cannot open ");
        puts_(dev);
        puts_("\n");
        sys_exit(1);
    }

    long r = sys_read(fd, mbr, SECTOR_SIZE);
    if (r != SECTOR_SIZE) {
        puts_("fdisk: short read on ");
        puts_(dev);
        puts_("\n");
        sys_close(fd);
        sys_exit(1);
    }

    int has_sig = (mbr[510] == 0x55 && mbr[511] == 0xAA);
    if (!has_sig)
        puts_("fdisk: no valid MBR signature (0x55AA); table may be blank\n");

    print_table(dev);
    puts_("\nCommands: p print  n new  d delete  t type  a boot  w write  q quit\n");

    int dirty = 0;
    for (;;) {
        puts_("fdisk> ");
        if (readline() == 0) continue;

        char c = line[0];
        if (c == 'q') {
            if (dirty)
                puts_("Quitting without writing; changes discarded.\n");
            break;
        } else if (c == 'p') {
            print_table(dev);
        } else if (c == 'a' || c == 'd' || c == 't' || c == 'n') {
            puts_("Partition number (1-4): ");
            if (readline() == 0) continue;
            unsigned int idx = parse_uint(line);
            if (!valid_index(idx)) { puts_("Invalid partition.\n"); continue; }
            unsigned char *e = entry((int)idx - 1);

            if (c == 'a') {
                e[0] = (e[0] == 0x80) ? 0x00 : 0x80;
                dirty = 1;
                puts_("Bootable flag toggled.\n");
            } else if (c == 'd') {
                for (int i = 0; i < PART_ENTRY; i++) e[i] = 0;
                dirty = 1;
                puts_("Partition cleared.\n");
            } else if (c == 't') {
                puts_("Type (hex, e.g. 0c): ");
                if (readline() == 0) continue;
                e[4] = (unsigned char)parse_uint(line);
                dirty = 1;
                puts_("Type set to ");
                puthex2(e[4]);
                puts_(" (");
                puts_(type_name(e[4]));
                puts_(")\n");
            } else { /* 'n' */
                puts_("Start LBA: ");
                if (readline() == 0) continue;
                unsigned int start = parse_uint(line);
                puts_("Size in sectors: ");
                if (readline() == 0) continue;
                unsigned int count = parse_uint(line);
                wr32(e + 8, start);
                wr32(e + 12, count);
                if (e[4] == 0x00) e[4] = 0x0C;   /* default to FAT32 LBA */
                /* CHS fields are legacy; mark "use LBA" sentinel. */
                e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
                e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
                dirty = 1;
                puts_("Partition set.\n");
            }
        } else if (c == 'w') {
            mbr[510] = 0x55;
            mbr[511] = 0xAA;
            if (sys_lseek(fd, 0, SEEK_SET) != 0) {
                puts_("fdisk: seek failed\n");
                continue;
            }
            long w = sys_write(fd, mbr, SECTOR_SIZE);
            if (w != SECTOR_SIZE) {
                puts_("fdisk: write failed\n");
            } else {
                dirty = 0;
                puts_("MBR written. Re-scan or remount to pick up changes.\n");
            }
        } else {
            puts_("Unknown command.\n");
        }
    }

    sys_close(fd);
    return 0;
}
