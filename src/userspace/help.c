#include "syscall.h"

static void puts_fd(int fd, const char *s)
{
    unsigned int len = 0;
    while (s[len]) len++;
    sys_write(fd, s, len);
}

#define PUTS(s) puts_fd(1, s)

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    PUTS("Makar OS - built-in commands\n");
    PUTS("\n");
    PUTS("Display:\n");
    PUTS("  clear                        clear the terminal\n");
    PUTS("  fgcol <colour>               set foreground colour\n");
    PUTS("  bgcol <colour>               set background colour\n");
    PUTS("  setmode <25|50|720p|1080p>   switch display mode\n");
    PUTS("\n");
    PUTS("System:\n");
    PUTS("  echo [args..]                print arguments\n");
    PUTS("  meminfo                      heap used/free\n");
    PUTS("  uptime                       ticks since boot\n");
    PUTS("  version                      build info and copyright\n");
    PUTS("  tasks                        list kernel tasks\n");
    PUTS("  shutdown                     power off (ACPI S5)\n");
    PUTS("  reboot                       reboot (ACPI/KBC)\n");
    PUTS("  ktest                        run in-kernel unit tests\n");
    PUTS("\n");
    PUTS("Disk:\n");
    PUTS("  lsdisks                      list detected ATA drives\n");
    PUTS("  lspart <drv>                 list partitions\n");
    PUTS("  mkpart <drv> <mbr|gpt>       create partition table\n");
    PUTS("  readsector <drv> <lba>       hex-dump one sector\n");
    PUTS("\n");
    PUTS("Filesystem (VFS: /hd for HDD, /cdrom for CD-ROM):\n");
    PUTS("  mkfs <drv> <part#>           format partition as FAT32\n");
    PUTS("  mount <drv> <part#>          mount FAT32 at /hd/\n");
    PUTS("  umount                       unmount FAT32\n");
    PUTS("  ls [path]                    list directory\n");
    PUTS("  cd <path>                    change directory\n");
    PUTS("  cat <file>                   print file contents\n");
    PUTS("  write <file> <text..>        create/overwrite file\n");
    PUTS("  touch <file>                 create empty file\n");
    PUTS("  cp <src> <dst>               copy a file\n");
    PUTS("  mkdir <path>                 create directory\n");
    PUTS("  isols <drv> [path]           list ISO9660 directory\n");
    PUTS("\n");
    PUTS("Installer:\n");
    PUTS("  install                      install Makar to a hard drive\n");
    PUTS("\n");
    PUTS("Boot:\n");
    PUTS("  chainload <drv> [lba]        boot a sector from disk\n");
    PUTS("\n");
    PUTS("Apps:\n");
    PUTS("  exec <path> [args..]         run an ELF32 executable\n");
    PUTS("  vix <file>                  interactive text editor\n");
    PUTS("  eject hdd|cdrom              unmount and eject\n");
    PUTS("\n");
    PUTS("Userland apps (exec /apps/<name>):\n");
    PUTS("  calc.elf                     expression calculator\n");
    PUTS("  echo.elf [args..]            echo arguments\n");
    PUTS("  hello.elf                    hello world\n");
    PUTS("  help.elf                     this help text\n");

    return 0;
}
