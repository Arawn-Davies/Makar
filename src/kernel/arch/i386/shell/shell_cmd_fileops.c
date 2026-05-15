/*
 * shell_cmd_fileops.c -- file operation shell commands (delete, move, rename).
 *
 * Commands: rm  rmdir  mv
 */

#include "shell_priv.h"

#include <kernel/tty.h>
#include <kernel/vfs.h>

static void cmd_rm(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("Usage: rm <file>...\n");
        return;
    }
    /* Iterate so wildcards (`rm *.tmp`) and explicit multi-arg both work. */
    for (int i = 1; i < argc; i++) {
        if (vfs_delete_file(argv[i]) != 0) {
            t_writestring("rm: cannot delete '");
            t_writestring(argv[i]);
            t_writestring("'\n");
        }
    }
}

static void cmd_rmdir(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("Usage: rmdir <directory>...\n");
        return;
    }
    for (int i = 1; i < argc; i++) {
        int r = vfs_delete_dir(argv[i]);
        if (r == -5) {
            t_writestring("rmdir: directory not empty: '");
            t_writestring(argv[i]);
            t_writestring("'\n");
        } else if (r != 0) {
            t_writestring("rmdir: cannot remove '");
            t_writestring(argv[i]);
            t_writestring("'\n");
        }
    }
}

static void cmd_mv(int argc, char **argv)
{
    if (argc < 3) {
        t_writestring("Usage: mv <src> <dst>\n");
        return;
    }
    if (vfs_rename(argv[1], argv[2]) != 0) {
        t_writestring("mv: cannot rename '");
        t_writestring(argv[1]);
        t_writestring("' to '");
        t_writestring(argv[2]);
        t_writestring("'\n");
    }
}

const shell_cmd_entry_t fileops_cmds[] = {
    { "rm",    cmd_rm    },
    { "rmdir", cmd_rmdir },
    { "mv",    cmd_mv    },
    { NULL, NULL }
};
