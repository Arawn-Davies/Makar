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

static void cmd_shutdown(int argc, char **argv)
{
    (void)argc; (void)argv;
    acpi_shutdown(); /* never returns */
}

static void cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    acpi_reboot(); /* never returns */
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

/* `verbose [on|off]` - toggle the tty-to-serial mirror.  Linux-equivalent
 * to flipping `console=ttyS0` on the kernel cmdline at runtime.  Without
 * an argument, just reports the current state.  Used both interactively
 * (debugging a remote system over COM1) and by ui_test scenarios that
 * need to grep shell output from the serial log. */
static void cmd_verbose(int argc, char **argv)
{
    if (argc >= 2) {
        if (strcmp(argv[1], "on") == 0)       g_serial_verbose = 1;
        else if (strcmp(argv[1], "off") == 0) g_serial_verbose = 0;
        else { t_writestring("Usage: verbose [on|off]\n"); return; }
    }
    t_writestring("serial verbose: ");
    t_writestring(g_serial_verbose ? "on\n" : "off\n");
}

/* ---------------------------------------------------------------------------
 * Module table
 * --------------------------------------------------------------------------- */

const shell_cmd_entry_t system_cmds[] = {
    { "echo",     cmd_echo     },
    { "meminfo",  cmd_meminfo  },
    { "uptime",   cmd_uptime   },
    { "tasks",    cmd_tasks    },
    { "shutdown", cmd_shutdown },
    { "reboot",   cmd_reboot   },
    { "panic",    cmd_panic    },
    { "ktest",    cmd_ktest    },
    { "verbose",  cmd_verbose  },
    { NULL, NULL }
};
