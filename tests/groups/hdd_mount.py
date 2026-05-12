"""HDD mount verification group.

Verifies that vfs_auto_mount() successfully mounted the FAT32 partition
from the boot HDD at /hd.

The ktest_bg group runs before this one and always advances execution to
keyboard_getchar() - guaranteeing that vfs_auto_mount() has already completed
and the inferior is stopped in normal task context.  This group therefore
checks fat32_mounted() directly without issuing another continue.

A pass confirms the full HDD boot path:
  grub-install core.img → Multiboot2 → IDE detected ATA drive →
  part_probe found MBR FAT32 partition → fat32_mount() accepted the BPB.
"""

import gdb

NAME = 'HDD Mount'


def run():
    # ktest_bg already stopped at keyboard_getchar (after vfs_auto_mount),
    # so fat32_mounted() can be called directly via an inferior function call.
    try:
        mounted = int(gdb.parse_and_eval('fat32_mounted()'))
    except gdb.error as exc:
        print('FAIL: could not evaluate fat32_mounted(): ' + str(exc),
              flush=True)
        return False

    if not mounted:
        print('FAIL: fat32_mounted() returned 0 - /hd was NOT mounted',
              flush=True)
        return False

    print('PASS: fat32_mounted() returned {} - /hd is mounted'.format(mounted),
          flush=True)
    print('GROUP PASS: ' + NAME, flush=True)
    return True
