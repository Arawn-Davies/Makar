"""HDD content verification group.

Checks that expected files exist on the FAT32 filesystem at /hd.
Runs after keyboard_getchar is reached (vfs_auto_mount has completed).
"""

import gdb

NAME = 'HDD content'

EXPECTED_FILES = [
    '/mnt/hd/boot/makar.kernel',
    '/mnt/hd/apps/hello.elf',
    '/mnt/hd/apps/calc.elf',
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
            print('FAIL: {} not found on HDD'.format(path), flush=True)
            return False

        print('PASS: {} exists'.format(path), flush=True)

    print('GROUP PASS: ' + NAME, flush=True)
    return True
