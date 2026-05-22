"""CD-ROM content verification group.

Checks that expected files exist on the ISO9660 filesystem at /mnt/cdrom.
Must run after ktest_bg (which breaks at keyboard_getchar, guaranteeing
vfs_auto_mount has completed).
"""

import gdb

NAME = 'CD-ROM content'

EXPECTED_FILES = [
    '/mnt/cdrom/boot/makar.kernel',
    '/mnt/cdrom/apps/hello.elf',
    '/mnt/cdrom/apps/calc.elf',
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
