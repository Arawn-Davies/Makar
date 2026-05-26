"""Boot-checkpoint group.

Verifies that every expected function is called during a normal kernel boot
by installing a silent breakpoint on each one.  The last breakpoint in the
list stops execution so the next group can take over.

Checkpoints must be listed in call order so that the auto-continue logic
works correctly: each breakpoint continues automatically until the very last
one, which returns True (stop) to hand control back to the runner.
"""

import gdb

NAME = 'Boot Checkpoints'

# Every function here must be reached during a normal boot.  Add new
# subsystems at the appropriate position in call order.
CHECKPOINTS = [
    'kernel_main',
    'terminal_initialize',
    'init_serial',
    'init_descriptor_tables',
    'init_debug_handlers',
    'pmm_init',
    'paging_init',
    'heap_init',
    'vesa_init',
    'vesa_tty_init',
    'init_timer',
    'keyboard_init',
    'ide_init',
    'tasking_init',
    'syscall_init',
    'acpi_init',
    # Normal boot drops into the ring-3 login shell via
    # user_shell_slot_entry; shell_run only runs when shell=rescue is
    # set on the cmdline (or as the per-VT fallback when /apps/sh.elf
    # fails to exec).  The stop point used to be shell_run -- moved
    # here when the userspace shell migration landed.
    'user_shell_slot_entry',
]


def run():
    hit = set()
    checkpoint_set = set(CHECKPOINTS)

    class CheckpointBreakpoint(gdb.Breakpoint):
        """Silent breakpoint that records a hit and auto-continues.

        Stops (returns True) only when every checkpoint has been reached,
        allowing a single gdb.execute('continue') to drive through the whole
        boot sequence.
        """

        def stop(self):
            hit.add(self.location)
            print('CHECKPOINT: ' + self.location, flush=True)
            return hit >= checkpoint_set

    for fn in CHECKPOINTS:
        bp = CheckpointBreakpoint(fn)
        bp.silent = True

    try:
        gdb.execute('continue')
    except gdb.error as exc:
        print('GDB error during boot checkpoints: ' + str(exc), flush=True)

    missing = [fn for fn in CHECKPOINTS if fn not in hit]
    if missing:
        for fn in missing:
            print('FAIL: checkpoint not reached: ' + fn, flush=True)
        return False

    print('GROUP PASS: ' + NAME, flush=True)
    return True
