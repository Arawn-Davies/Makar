"""/root home-dir verification group.

Verifies that vfs_ensure_root_home() (called from kernel_main after
vfs_auto_mount) successfully mkdir'd /root on the HDD rootfs.

Only meaningful on the HDD test path: vfs_ensure_root_home is a no-op
when the rootfs backend is ISO9660 (read-only CD boot).  Asserts
vfs_file_exists("/root") returns 1.
"""

import gdb

NAME = '/root home dir'


def run():
    try:
        result = int(gdb.parse_and_eval('vfs_file_exists("/root")'))
    except gdb.error as exc:
        print('FAIL: could not evaluate vfs_file_exists("/root"): ' + str(exc),
              flush=True)
        return False

    if result != 1:
        print('FAIL: /root not present on HDD rootfs '
              '(vfs_ensure_root_home() did not mkdir it)', flush=True)
        return False

    print('PASS: /root exists on HDD rootfs', flush=True)
    print('GROUP PASS: ' + NAME, flush=True)
    return True
