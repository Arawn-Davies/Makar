/*
 * shell_help.c -- help and version built-in commands.
 */

#include "shell_priv.h"

#include <kernel/tty.h>

void cmd_help(void)
{
    t_writestring(COPYRIGHT "\n");
    t_writestring("Commands:\n");
    t_writestring("  help                         - list commands\n");
    t_writestring("  clear                        - clear the terminal\n");
    t_writestring("  echo [args..]                - print arguments\n");
    t_writestring("  meminfo                      - print heap used/free\n");
    t_writestring("  uptime                       - ticks since boot\n");
    t_writestring("  version                      - show build info and copyright\n");
    t_writestring("  tasks                        - list kernel tasks\n");
    t_writestring("  lsdisks                      - list detected ATA drives\n");
    t_writestring("  lspart <drv>                 - list partitions (MBR or GPT)\n");
    t_writestring("  mkpart <drv> <mbr|gpt>       - create a partition table\n");
    t_writestring("  readsector <drv> <lba>       - hex-dump one sector\n");
    t_writestring("  setmode <25|50>              - switch between 80x25 and 80x50\n");
    t_writestring("  shutdown                     - power off the system (ACPI S5)\n");
    t_writestring("  reboot                       - reboot the system (ACPI/KBC)\n");
    t_writestring("  ktest                        - run in-kernel unit tests\n");
    t_writestring("Filesystem (universal VFS: /hd for HDD, /cdrom for CD-ROM):\n");
    t_writestring("  mkfs <drv> <part#>           - format partition as FAT32\n");
    t_writestring("  mount <drv> <part#>          - mount FAT32 partition at /hd/\n");
    t_writestring("  umount                       - unmount the FAT32 volume\n");
    t_writestring("  ls [path]                    - list directory (/hd/.. or /cdrom/..)\n");
    t_writestring("  cd <path>                    - change directory\n");
    t_writestring("  cat <file>                   - print file contents\n");
    t_writestring("  mkdir <path>                 - create directory (FAT32 only)\n");
    t_writestring("  isols <drv> [path]           - list directory on an ISO9660 CD-ROM\n");
    t_writestring("Installer:\n");
    t_writestring("  install                      - install Makar OS to a hard drive\n");
    t_writestring("Development:\n");
    t_writestring("  ring3test                    - spawn a ring-3 smoke test process\n");
}

void cmd_version(void)
{
    t_writestring("Makar -- version " SHELL_VERSION
                  ", build: " BUILD_DATE " " BUILD_TIME "\n");
    t_writestring("The GCC/C++ sibling of Medli\n");
    t_writestring(COPYRIGHT "\n");
    t_writestring("Released under the BSD-3 Clause Clear license\n");
}
