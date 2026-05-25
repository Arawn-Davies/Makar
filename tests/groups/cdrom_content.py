"""CD-ROM content verification group.

Checks that expected files exist on the ISO9660 boot medium.  Paths use
the rootfs-elected form (/...) which routes to /mnt/cdrom when the CD
is the active rootfs.  Must run after ktest_bg (which breaks at
keyboard_getchar, guaranteeing vfs_auto_mount has completed).
"""

import gdb

NAME = 'CD-ROM content'

EXPECTED_FILES = [
    '/boot/makar.kernel',
    '/apps/hello.elf',
    '/apps/calc.elf',
    '/apps/tcc.elf',
    '/usr/lib/libc.a',
    '/usr/lib/crt1.o',
    '/usr/include/syscall.h',
    '/usr/lib/tcc/libtcc1.a',
]


def run():
    for path in EXPECTED_FILES:
        try:
            result = int(gdb.parse_and_eval(
                'vfs_file_exists("{}")'.format(path)))
        except gdb.error as exc:
            print('FAIL: could not check {}: {}'.format(path, exc),
                  flush=True)
            return False

        if result != 1:
            print('FAIL: {} not found on CD-ROM'.format(path), flush=True)
            return False

        print('PASS: {} exists'.format(path), flush=True)

    print('GROUP PASS: ' + NAME, flush=True)
    return True
