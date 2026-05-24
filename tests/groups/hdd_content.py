"""HDD content verification group.

Checks that expected files exist on the rootfs partition (mounted at
/mnt/root after vfs_auto_mount, and elevated to / via the rootfs election).
Runs after keyboard_getchar is reached (vfs_auto_mount has completed).
"""

import gdb

NAME = 'HDD content'

EXPECTED_FILES = [
    '/mnt/root/boot/makar.kernel',
    '/mnt/root/apps/hello.elf',
    '/mnt/root/apps/calc.elf',
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
