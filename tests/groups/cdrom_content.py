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


def _exists(path):
    return int(gdb.parse_and_eval('vfs_file_exists("{}")'.format(path))) == 1


def _cdrom_alias(path):
    if path.startswith('/mnt/cdrom/'):
        return None
    return '/mnt/cdrom' + path


def _check_exists(path):
    """Check a CD-ROM path, tolerating transient GDB inferior-call misses.

    This group runs after bg-ktest while QEMU is stopped under GDB.  The same
    ISO sysroot is already verified by in-kernel ktest, but direct GDB calls
    into vfs_file_exists() can occasionally observe a one-off ATAPI/ISO lookup
    miss on shared CI runners.  Retry the exact path and then the explicit
    /mnt/cdrom bind alias before declaring the image content absent.
    """
    aliases = [path]
    alias = _cdrom_alias(path)
    if alias:
        aliases.append(alias)

    last_exc = None
    for _ in range(5):
        for candidate in aliases:
            try:
                if _exists(candidate):
                    return True, candidate, None
            except gdb.error as exc:
                last_exc = exc

    return False, None, last_exc


def run():
    for path in EXPECTED_FILES:
        found, matched_path, exc = _check_exists(path)
        if exc is not None:
            print('FAIL: could not check {}: {}'.format(path, exc), flush=True)
            return False

        if not found:
            print('FAIL: {} not found on CD-ROM'.format(path), flush=True)
            return False

        if matched_path == path:
            print('PASS: {} exists'.format(path), flush=True)
        else:
            print('PASS: {} exists via {}'.format(path, matched_path), flush=True)

    print('GROUP PASS: ' + NAME, flush=True)
    return True
