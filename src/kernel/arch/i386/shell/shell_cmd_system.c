/*
 * shell_cmd_system.c -- system information and control shell commands.
 *
 * Commands: echo  meminfo  uptime  tasks  shutdown  reboot  panic  ktest
 */

#include "shell_priv.h"

#include <kernel/tty.h>
#include <kernel/timer.h>
#include <kernel/heap.h>
#include <kernel/task.h>
#include <kernel/acpi.h>
#include <kernel/debug.h>
#include <kernel/ktest.h>
#include <kernel/serial.h>
#include <kernel/vfs.h>
#include <kernel/installer.h>
#include <kernel/admin.h>

static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            t_putchar(' ');
        t_writestring(argv[i]);
    }
    t_putchar('\n');
}

static void cmd_meminfo(int argc, char **argv)
{
    (void)argc; (void)argv;
    t_writestring("heap used: ");
    t_dec((uint32_t)heap_used());
    t_writestring(" bytes\n");
    t_writestring("heap free: ");
    t_dec((uint32_t)heap_free());
    t_writestring(" bytes\n");
}

static void cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Timer runs at 100 Hz, so 1 second = 100 ticks. Decompose into
     * h:mm:ss, with the leftover tick remainder shown for sub-second
     * precision. */
    uint32_t ticks   = timer_get_ticks();
    uint32_t total_s = ticks / 100u;
    uint32_t rem     = ticks % 100u;          /* 0..99, hundredths of a sec */
    uint32_t hours   = total_s / 3600u;
    uint32_t minutes = (total_s % 3600u) / 60u;
    uint32_t seconds = total_s % 60u;

    t_writestring("uptime: ");
    t_dec(hours);   t_writestring("h ");
    t_dec(minutes); t_writestring("m ");
    t_dec(seconds); t_writestring(".");
    /* Pad hundredths to two digits without a printf. */
    if (rem < 10) t_putchar('0');
    t_dec(rem);
    t_writestring("s  (");
    t_dec(ticks);
    t_writestring(" ticks @ 100 Hz)\n");
}

static void cmd_tasks(int argc, char **argv)
{
    (void)argc; (void)argv;
    static const char * const state_names[] = { "ready", "running", "dead" };
    int n = task_count();

    t_writestring("Tasks (");
    t_dec((uint32_t)n);
    t_writestring(" total):\n");

    for (int i = 0; i < n; i++) {
        task_t *t = task_get(i);
        if (!t)
            continue;
        t_writestring("  [");
        t_dec((uint32_t)i);
        t_writestring("] ");
        t_writestring(t->name ? t->name : "(unnamed)");
        t_writestring(" - ");
        t_writestring(state_names[t->state]);
        t_putchar('\n');
    }
}

/* admin_shutdown / admin_reboot -- called by both the rescue shell and
 * the userspace shell via SYS_SHUTDOWN / SYS_REBOOT.  Never return on
 * success (control passes to ACPI), so the int return is just for the
 * privilege-check failure path. */
int admin_shutdown(void)
{
    vfs_prepare_shutdown();  /* flush + unmount before power-off */
    acpi_shutdown();         /* never returns */
    return 0;                /* unreachable */
}

int admin_reboot(void)
{
    vfs_prepare_shutdown();
    acpi_reboot();
    return 0;
}

/* admin_install -- launch the Limine installer TUI from the calling task.
 * Mirrors `cmd_install` (shell_cmd_apps.c) so the in-kernel rescue shell
 * and the userspace shell reach the same code via SYS_INSTALL.  The
 * installer paints the framebuffer; on a serial-only boot the user can
 * still type its keys via the focused TTY's keyboard ring. */
int admin_install(void)
{
    installer_run();
    return 0;
}

static void cmd_shutdown(int argc, char **argv)
{
    (void)argc; (void)argv;
    admin_shutdown();
}

static void cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    admin_reboot();
}

static void cmd_panic(int argc, char **argv)
{
    const char *msg = (argc >= 2) ? argv[1] : "Shell-requested panic";
    KPANIC(msg); /* never returns */
}

static void cmd_ktest(int argc, char **argv)
{
    (void)argc; (void)argv;
    ktest_run_all();
}

/* `sched_quantum [n]` - read or set the preemption quantum (PIT ticks
 * per slice).  No argument prints the current value; an argument 1..100
 * sets it.  Slice 9 phase 3 -- pairs with the per-task kticks counter
 * in /proc/tasks so an operator can see the effect of changing the
 * quantum on individual tasks. */
static int parse_uint_dec(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    if (!*s) return -1;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10u + (uint32_t)(*p - '0');
        if (v > 1000000u) return -1;
    }
    *out = v;
    return 0;
}

/* admin_sched_quantum -- get or set the scheduling quantum (PIT ticks).
 * `new_value < 0` queries (no mutation); 0 < new_value <= SCHED_QUANTUM_MAX
 * sets.  Always prints the resulting state.  Returns the post-call value
 * on success, -1 on out-of-range. */
int admin_sched_quantum(int new_value)
{
    if (new_value >= 0) {
        if ((uint32_t)new_value < SCHED_QUANTUM_MIN ||
            (uint32_t)new_value > SCHED_QUANTUM_MAX) {
            t_writestring("Usage: sched_quantum [");
            t_dec(SCHED_QUANTUM_MIN);
            t_writestring("..");
            t_dec(SCHED_QUANTUM_MAX);
            t_writestring("]\n");
            return -1;
        }
        g_sched_quantum = (uint32_t)new_value;
    }
    t_writestring("sched_quantum: ");
    t_dec(g_sched_quantum);
    t_writestring(" ticks (");
    t_dec(g_sched_quantum * 10u);
    t_writestring(" ms @ 100 Hz)\n");
    return (int)g_sched_quantum;
}

static void cmd_sched_quantum(int argc, char **argv)
{
    if (argc >= 2) {
        uint32_t n;
        if (parse_uint_dec(argv[1], &n) != 0) {
            t_writestring("Usage: sched_quantum [");
            t_dec(SCHED_QUANTUM_MIN);
            t_writestring("..");
            t_dec(SCHED_QUANTUM_MAX);
            t_writestring("]\n");
            return;
        }
        admin_sched_quantum((int)n);
    } else {
        admin_sched_quantum(-1);
    }
}

/* `verbose [on|off]` - toggle the tty-to-serial mirror.  Linux-equivalent
 * to flipping `console=ttyS0` on the kernel cmdline at runtime.  Without
 * an argument, just reports the current state.  Used both interactively
 * (debugging a remote system over COM1) and by ui_test scenarios that
 * need to grep shell output from the serial log. */
/* admin_verbose: 1 = on, 0 = off, -1 = query.  Always prints the final
 * state.  Returns the post-call state (0 or 1) or -1 on bad input. */
int admin_verbose(int onoff)
{
    if (onoff == 0 || onoff == 1) g_serial_verbose = onoff;
    else if (onoff != -1) return -1;
    t_writestring("serial verbose: ");
    t_writestring(g_serial_verbose ? "on\n" : "off\n");
    return g_serial_verbose;
}

static void cmd_verbose(int argc, char **argv)
{
    if (argc >= 2) {
        if (strcmp(argv[1], "on") == 0)       admin_verbose(1);
        else if (strcmp(argv[1], "off") == 0) admin_verbose(0);
        else { t_writestring("Usage: verbose [on|off]\n"); return; }
    } else {
        admin_verbose(-1);
    }
}

/* ---------------------------------------------------------------------------
 * Module table
 * --------------------------------------------------------------------------- */

/* date -- one-line wall clock readout, scriptable.
 *
 * Reads /proc/rtc (procfs already formats YYYY-MM-DD HH:MM:SS from CMOS).
 * Unlike clock.elf which takes over the framebuffer, this is a single
 * printf -- safe to call inside a shell script or a backtick context
 * once we have command substitution. */
static void cmd_date(int argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[64];
    uint32_t got = 0;
    if (vfs_read_file("/proc/rtc", buf, sizeof(buf) - 1, &got) != 0 || got == 0) {
        t_writestring("date: /proc/rtc unavailable\n");
        return;
    }
    buf[got] = '\0';
    /* /proc/rtc emits "YYYY-MM-DD HH:MM:SS\n"; pass through verbatim. */
    t_writestring(buf);
    if (got > 0 && buf[got - 1] != '\n') t_putchar('\n');
}

const shell_cmd_entry_t system_cmds[] = {
    { "datetime", cmd_date     },   /* canonical: one line "YYYY-MM-DD HH:MM:SS" */
    { "date",     cmd_date     },   /* alias */
    { "time",     cmd_date     },   /* alias */
    { "echo",     cmd_echo     },
    { "meminfo",  cmd_meminfo  },
    { "uptime",   cmd_uptime   },
    { "tasks",    cmd_tasks    },
    { "shutdown", cmd_shutdown },
    { "reboot",   cmd_reboot   },
    { "panic",    cmd_panic    },
    { "ktest",    cmd_ktest    },
    { "verbose",  cmd_verbose  },
    { "sched_quantum", cmd_sched_quantum },
    { NULL, NULL }
};
