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
 * The `n` (new) command accepts friendly sizes as well as raw sector
 * counts: `max` (rest of disk), `N%` (percent of free space), `NM`/`NG`
 * (MiB/GiB), or a bare sector count.  Start LBA defaults to the first
 * 1 MiB-aligned sector past the last partition (Enter to accept).
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

/* parse_hex - always interpret the token as hexadecimal (optional 0x prefix).
 * Used for partition type codes, where the prompt advertises hex but operators
 * type bare digits like "83" expecting 0x83 (Linux), not decimal 83 (= 0x53). */
static unsigned int parse_hex(const char *s)
{
    unsigned int v = 0;
    while (*s == ' ') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
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

/* Sectors per MiB / GiB (512-byte sectors). */
#define SECT_PER_MIB  2048u
#define SECT_PER_GIB  (2048u * 1024u)

/* ci3 - case-insensitive 3-char match against a lowercase literal. */
static int ci3(const char *s, char a, char b, char c)
{
    char x = s[0], y = s[1], z = s[2];
    if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
    if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
    if (z >= 'A' && z <= 'Z') z = (char)(z + 32);
    return x == a && y == b && z == c;
}

/*
 * default_start - first 1 MiB-aligned LBA past every populated partition,
 * never below 2048 (the GRUB/limine-safe embedding gap).
 */
static unsigned int default_start(void)
{
    unsigned int s = 2048u;
    for (int i = 0; i < NUM_PARTS; i++) {
        unsigned char *e = entry(i);
        unsigned int cnt = le32(e + 12);
        if (cnt == 0) continue;
        unsigned int end = le32(e + 8) + cnt;
        if (end > s) s = end;
    }
    return (s + 2047u) & ~2047u;        /* round up to 1 MiB */
}

/*
 * parse_size - turn a friendly size token into a sector count.
 *
 *   max            all space from `start` to the end of the disk
 *   N%             percentage of the whole disk (capped at 100%); the
 *                  caller clamps start+count to the disk end, so an
 *                  over-allocation (e.g. 30%+30%+30%+20%) is caught there
 *   N / NM / NMiB  N MiB
 *   NG / NGiB      N GiB
 *   N (bare)       N raw 512-byte sectors
 *
 * `start` and `total` are in sectors.  Returns 0 on parse failure / empty.
 * All arithmetic is 32-bit (userspace links -nostdlib, no __udivdi3).
 */
static unsigned int parse_size(const char *s, unsigned int start, unsigned int total)
{
    while (*s == ' ') s++;

    if (ci3(s, 'm', 'a', 'x'))
        return (total > start) ? total - start : 0;

    unsigned int v = 0;
    int got = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10u + (unsigned int)(*s++ - '0'); got = 1; }
    if (!got) return 0;
    while (*s == ' ') s++;

    char u = *s;
    if (u == '%') {
        if (v >= 100u) return total;
        /* percent of the whole disk; split to dodge 32-bit overflow on
         * large disks.  The caller clamps start+count to the disk end. */
        return (total / 100u) * v + ((total % 100u) * v) / 100u;
    }
    if (u == 'g' || u == 'G') return v * SECT_PER_GIB;
    if (u == 'm' || u == 'M') return v * SECT_PER_MIB;
    return v;                            /* bare sectors */
}

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

    /* Total disk size in sectors, via SEEK_END (block-device fd reports its
     * byte size).  0 if the kernel can't tell us; max/% then unavailable. */
    long dev_bytes = sys_lseek(fd, 0, SEEK_END);
    unsigned int total_sectors = (dev_bytes > 0) ? (unsigned int)(dev_bytes / SECTOR_SIZE) : 0u;
    sys_lseek(fd, 0, SEEK_SET);

    if (total_sectors) {
        puts_("Disk size: ");
        putu(total_sectors);
        puts_(" sectors (");
        putu(total_sectors / SECT_PER_MIB);
        puts_(" MiB)\n");
    }

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
                e[4] = (unsigned char)parse_hex(line);
                dirty = 1;
                puts_("Type set to ");
                puthex2(e[4]);
                puts_(" (");
                puts_(type_name(e[4]));
                puts_(")\n");
            } else { /* 'n' */
                unsigned int dstart = default_start();
                puts_("Start LBA [");
                putu(dstart);
                puts_("] (Enter for default): ");
                unsigned int start = (readline() == 0) ? dstart : parse_uint(line);

                puts_("Size (sectors, NM, NG, N%, or max): ");
                if (readline() == 0) continue;
                unsigned int count = parse_size(line, start, total_sectors);
                if (count == 0) {
                    puts_("Invalid or zero size.\n");
                    continue;
                }
                /* Clamp to the disk so we never describe sectors past the
                 * end.  Overflow-safe: compare against remaining space rather
                 * than `start + count` (which can wrap uint32 on huge sizes). */
                if (total_sectors && start >= total_sectors) {
                    puts_("Start past end of disk.\n");
                    continue;
                }
                if (total_sectors && count > total_sectors - start) {
                    count = total_sectors - start;
                    puts_("  (clamped to end of disk: ");
                    putu(count);
                    puts_(" sectors)\n");
                }
                wr32(e + 8, start);
                wr32(e + 12, count);
                if (e[4] == 0x00) e[4] = 0x0C;   /* default to FAT32 LBA */
                /* CHS fields are legacy; mark "use LBA" sentinel. */
                e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
                e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
                dirty = 1;
                puts_("Partition set: start ");
                putu(start);
                puts_(", ");
                putu(count);
                puts_(" sectors (");
                putu(count / SECT_PER_MIB);
                puts_(" MiB).\n");
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
