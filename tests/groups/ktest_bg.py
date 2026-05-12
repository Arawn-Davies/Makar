"""Background ktest verification group.

Verifies that the background ktest task completed successfully.

Always advances to the first keyboard_getchar call (shell REPL entry) so that
subsequent groups get a clean, consistent stopping point outside any ISR frame.
This avoids "Cannot execute this command while the target is running" errors
that occur when GDB is stopped inside a timer ISR (timer_callback) and the
next group tries to install a new breakpoint.
"""

import gdb

NAME = 'Background ktest'


def run():
    # Always advance to keyboard_getchar - this guarantees:
    #   1. The shell has exited the ktest wait loop (ktest_bg_done == 1).
    #   2. vfs_init() + vfs_auto_mount() have completed.
    #   3. The inferior is stopped in normal task context (not an ISR frame),
    #      so subsequent groups can safely install breakpoints.
    bp = gdb.Breakpoint('keyboard_getchar', internal=True, temporary=True)
    bp.silent = True
    try:
        gdb.execute('continue')
    except gdb.error as exc:
        print('FAIL: GDB error waiting for keyboard_getchar: ' + str(exc),
              flush=True)
        return False

    try:
        done = int(gdb.parse_and_eval('ktest_bg_done'))
    except gdb.error as exc:
        print('FAIL: could not evaluate ktest_bg_done: ' + str(exc),
              flush=True)
        return False

    if done != 1:
        print('FAIL: ktest_bg_done = {} (expected 1)'.format(done),
              flush=True)
        return False

    print('PASS: ktest_bg_done = 1 - background tests completed', flush=True)
    print('GROUP PASS: ' + NAME, flush=True)
    return True
