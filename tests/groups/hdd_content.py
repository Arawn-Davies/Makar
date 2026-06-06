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


# vfs_file_exists() runs here as a GDB *inferior function call*: the stopped
# guest executes the kernel's FAT32 directory walk, which issues a real ATA
# read.  Driven that way (mid-breakpoint, off the normal scheduler) a directory
# /FAT sector read can transiently miss, so the lookup returns 0 for a file that
# IS on the image -- historically a flaky "X.elf not found", a different file
# each run, cleared by re-running the whole job.  Re-issuing the call re-reads
# the sector, so a short retry deflakes it without masking a genuinely absent
# file (which fails all attempts).
RETRIES = 8


def _exists(path):
    last = 0
    for _ in range(RETRIES):
        try:
            last = int(gdb.parse_and_eval(
                'vfs_file_exists("{}")'.format(path)))
        except gdb.error as exc:
            print('FAIL: could not check {}: {}'.format(path, exc), flush=True)
            return None
        if last == 1:
            return 1
    return last


def run():
    for path in EXPECTED_FILES:
        result = _exists(path)
        if result is None:
            return False
        if result != 1:
            print('FAIL: {} not found on HDD (after {} retries)'.format(
                path, RETRIES), flush=True)
            return False

        print('PASS: {} exists'.format(path), flush=True)

    print('GROUP PASS: ' + NAME, flush=True)
    return True
