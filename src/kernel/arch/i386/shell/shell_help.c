/*
 * shell_help.c -- help and version built-in commands.
 *
 * 'help' redirects to 'lsman'.  Full command reference lives in
 * shell_cmd_man.c so it stays in one place.
 */

#include "shell_priv.h"
#include <kernel/tty.h>
#include <kernel/version.h>

void cmd_lsman(int argc, char **argv);

static void cmd_help(int argc, char **argv)
{
    cmd_lsman(argc, argv);
}

/* version - report the linked component versions.  The kernel version is
 * the single-source MAKAR_VERSION (<kernel/version.h>); the shell version
 * is tracked independently (SHELL_VERSION) since it's a separate component. */
static void cmd_version(int argc, char **argv)
{
    (void)argc; (void)argv;
    t_writestring("Makar kernel " MAKAR_VERSION "\n");
    t_writestring("Makar shell  " SHELL_VERSION "\n");
    t_writestring("build: " BUILD_DATE " " BUILD_TIME "\n");
}

/* about - identity, credits, and licence (moved off the boot banner). */
static void cmd_about(int argc, char **argv)
{
    (void)argc; (void)argv;
    t_writestring("Makar " MAKAR_VERSION " -- The GCC/C++ sibling of Medli\n");
    t_writestring(COPYRIGHT "\n");
    t_writestring("Released under the BSD-3 Clause Clear license\n");
}

const shell_cmd_entry_t help_cmds[] = {
    { "help",    cmd_help    },
    { "version", cmd_version },
    { "about",   cmd_about   },
    { NULL, NULL }
};
