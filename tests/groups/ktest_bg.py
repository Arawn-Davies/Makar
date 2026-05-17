"""Background ktest verification group.

Verifies that the background ktest task completed successfully.

Breaks at ktest_bg_marker -- a noinline hook called immediately after
ktest_bg_done is set to 1.  This decouples the assertion from the shell
task's loading-screen wall-clock, which under TCG could race the GDB
step timeout (the previous design broke at the shell's first
keyboard_getchar call, which only ran AFTER bg ktest finished AND the
shell drained poll AND printed the banner).

The wall-clock budget is owned by the outer `timeout` wrapping the GDB
session in run.sh (300 s as of fix/gdb-bg-ktest-flake -- bumped from
120 s after release CI run 25948150516 was killed at exit 124 while
bg-ktest was still running suites under TCG on a shared GitHub
runner).  Per-suite progress is already written to the serial log by
ktest_bg_task's RUN macro, and gdb-serial.log is uploaded as a CI
artifact, so a slow boot is diagnosable without GDB-side polling.

Subsequent groups (CD-ROM content) only read variables; they don't
`continue` the inferior, so it's fine to leave it stopped here instead
of advancing into the shell.
"""

import gdb

NAME = 'Background ktest'


def run():
    bp = gdb.Breakpoint('ktest_bg_marker', internal=True, temporary=True)
    bp.silent = True
    try:
        gdb.execute('continue')
    except gdb.error as exc:
        print('FAIL: GDB error waiting for ktest_bg_marker: ' + str(exc),
              flush=True)
        return False

    try:
        done = int(gdb.parse_and_eval('ktest_bg_done'))
    except gdb.error as exc:
        print('FAIL: could not evaluate ktest_bg_done: ' + str(exc),
              flush=True)
        return False

    if done != 1:
        try:
            completed = int(gdb.parse_and_eval('ktest_bg_completed'))
            total = int(gdb.parse_and_eval('ktest_bg_total'))
            print('       last progress: ktest_bg_completed = {}/{}'.format(
                completed, total), flush=True)
        except gdb.error:
            pass
        print('FAIL: ktest_bg_done = {} (expected 1)'.format(done),
              flush=True)
        return False

    print('PASS: ktest_bg_done = 1 - background tests completed', flush=True)
    print('GROUP PASS: ' + NAME, flush=True)
    return True
