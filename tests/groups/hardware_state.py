"""Hardware-state group.

Verifies CPU register state and liveness after the boot sequence:
  1. CR0.PG (bit 31) must be set  – paging was enabled.
  2. CR3 must be non-zero         – page directory is loaded.
  3. timer_callback must fire     – PIT is ticking (kernel is alive).

This group is entered with execution stopped at the last boot checkpoint
(`user_shell_slot_entry` on normal boot; was `shell_run` pre-userspace-
shell-migration).  It reads CR0/CR3 while stopped there, then installs a
one-shot breakpoint on timer_callback and continues.  The shell task
blocks waiting for ktest_bg_done / focus / keyboard input; during that
spin-wait the PIT fires and timer_callback is reached, confirming the
PIT is alive.
"""

import gdb

NAME = 'Hardware State'


def run():
    # --- CR0.PG -----------------------------------------------------------
    cr0 = int(gdb.parse_and_eval('$cr0')) & 0xFFFFFFFF
    if cr0 & 0x80000000:
        print('PASS: CR0.PG is set (paging enabled, CR0=0x{:08X})'.format(cr0),
              flush=True)
    else:
        print('FAIL: CR0.PG not set (CR0=0x{:08X})'.format(cr0), flush=True)
        return False

    # --- CR3 --------------------------------------------------------------
    cr3 = int(gdb.parse_and_eval('$cr3')) & 0xFFFFFFFF
    if cr3 != 0:
        print('PASS: CR3 is non-zero (page directory at 0x{:08X})'.format(cr3),
              flush=True)
    else:
        print('FAIL: CR3 is zero (page directory not loaded)', flush=True)
        return False

    # --- Timer callback ---------------------------------------------------
    # Continuing from kernel_post_boot entry causes ksleep(50) to run,
    # during which the PIT fires at least once and calls timer_callback.
    timer_hit = [False]

    class TimerBreakpoint(gdb.Breakpoint):
        def stop(self):
            timer_hit[0] = True
            try:
                tick = int(gdb.parse_and_eval('tick'))
                print('PASS: timer_callback fired (tick={})'.format(tick),
                      flush=True)
            except gdb.error:
                print('PASS: timer_callback fired', flush=True)
            return True  # stop so the next group can take over

    tbp = TimerBreakpoint('timer_callback', internal=True)
    tbp.silent = True
    try:
        gdb.execute('continue')
    except gdb.error as exc:
        print('GDB error waiting for timer_callback: ' + str(exc), flush=True)
    tbp.delete()

    if not timer_hit[0]:
        print('FAIL: timer_callback never fired (PIT not running?)', flush=True)
        return False

    print('GROUP PASS: ' + NAME, flush=True)
    return True
