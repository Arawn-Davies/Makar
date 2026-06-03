/*
 * makbox.c -- Makar busybox: multicall userspace utility.
 *
 * The shell exec's this with argv[0]="makbox" argv[1]=<applet> argv[2..]=args.
 * Applets: ls, cat, cp, mv, rm, mkdir, rmdir, echo, pwd.
 * cd remains a shell builtin because parent-state mutation belongs in the
 * shell, like bash.
 */

#include "syscall.h"

static unsigned int str_len(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

static int streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static void write_fd(int fd, const char *s)
{
    sys_write(fd, s, str_len(s));
}

static int cmd_ls(int argc, char **argv)
{
    static char buf[4096];

    if (argc < 2) {
        int n = sys_ls_dir(".", buf, (unsigned int)sizeof(buf));
        if (n < 0) { write_fd(1, "ls: cannot list directory\n"); return 1; }
        if (n == 0) write_fd(1, "(empty)\n");
        else        sys_write(1, buf, (unsigned int)n);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (argc > 2) {
            if (i > 1) write_fd(1, "\n");
            write_fd(1, argv[i]);
            write_fd(1, ":\n");
        }
        int n = sys_ls_dir(argv[i], buf, (unsigned int)sizeof(buf));
        if (n < 0) {
            write_fd(1, "ls: cannot list '");
            write_fd(1, argv[i]);
            write_fd(1, "'\n");
            return 1;
        }
        if (n == 0) write_fd(1, "(empty)\n");
        else        sys_write(1, buf, (unsigned int)n);
    }
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    static unsigned char buf[4096];

    if (argc < 2) {
        write_fd(1, "Usage: cat <file>...\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) {
            write_fd(1, "cat: cannot open '");
            write_fd(1, argv[i]);
            write_fd(1, "'\n");
            return 1;
        }
        while (1) {
            long n = sys_read(fd, buf, (unsigned int)sizeof(buf));
            if (n < 0) {
                write_fd(1, "cat: read error on '");
                write_fd(1, argv[i]);
                write_fd(1, "'\n");
                sys_close(fd);
                return 1;
            }
            if (n == 0) break;
            for (long off = 0; off < n; ) {
                long chunk = n - off;
                if (chunk > 256) chunk = 256;
                sys_write(1, (const char *)buf + off, (unsigned int)chunk);
                off += chunk;
                sys_yield();
            }
        }
        sys_close(fd);
    }
    return 0;
}

/* 64 KiB static buffer -- matches the kernel's max file size for cp. */
#define FSUTIL_CP_BUF_SIZE 65536u
static unsigned char s_cp_buf[FSUTIL_CP_BUF_SIZE];
static unsigned char s_cp_overflow_byte;

static int cmd_cp(int argc, char **argv)
{
    if (argc < 3) {
        write_fd(1, "Usage: cp <src> <dst>\n");
        return 1;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    int fd = sys_open(src, O_RDONLY);
    if (fd < 0) {
        write_fd(1, "cp: cannot open '");
        write_fd(1, src);
        write_fd(1, "'\n");
        return 1;
    }

    long n = sys_read(fd, s_cp_buf, (unsigned int)sizeof(s_cp_buf));
    if (n < 0) {
        write_fd(1, "cp: read error on '");
        write_fd(1, src);
        write_fd(1, "'\n");
        sys_close(fd);
        return 1;
    }

    /* Probe one extra byte to detect oversize sources rather than silently
     * truncating to 64 KiB. */
    if ((unsigned long)n == (unsigned long)sizeof(s_cp_buf)) {
        long more = sys_read(fd, &s_cp_overflow_byte, 1u);
        if (more > 0) {
            write_fd(1, "cp: source too large (max 64 KiB)\n");
            sys_close(fd);
            return 1;
        }
    }
    sys_close(fd);

    if (sys_write_file(dst, s_cp_buf, (unsigned int)n) != 0) {
        write_fd(1, "cp: cannot write '");
        write_fd(1, dst);
        write_fd(1, "'\n");
        return 1;
    }
    return 0;
}

static int cmd_mv(int argc, char **argv)
{
    if (argc < 3) {
        write_fd(1, "Usage: mv <src> <dst>\n");
        return 1;
    }
    if (sys_rename_file(argv[1], argv[2]) != 0) {
        write_fd(1, "mv: cannot rename '");
        write_fd(1, argv[1]);
        write_fd(1, "' to '");
        write_fd(1, argv[2]);
        write_fd(1, "'\n");
        return 1;
    }
    return 0;
}

static int cmd_pwd(int argc, char **argv)
{
    (void)argc; (void)argv;
    static char buf[256];
    int n = sys_getcwd(buf, sizeof(buf));
    if (n < 0) {
        write_fd(1, "pwd: getcwd failed\n");
        return 1;
    }
    /* Serial-only provenance tag: lets the in-guest test drivers prove makbox's pwd ran
     * (not a stale shell builtin) without changing visible output. */
    sys_write_serial("[makbox:pwd] ", 13);
    sys_write_serial(buf, (unsigned int)n);
    sys_write_serial("\n", 1);
    sys_write(1, buf, (unsigned int)n);
    sys_write(1, "\n", 1);
    return 0;
}

static int cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) sys_write(1, " ", 1);
        sys_write(1, argv[i], str_len(argv[i]));
    }
    sys_write(1, "\n", 1);
    return 0;
}

static int cmd_rm(int argc, char **argv)
{
    int dir_mode = 0;
    int argi = 1;

    if (argc >= 2 && argv[1][0] == '-' &&
        argv[1][1] == 'd' && argv[1][2] == '\0') {
        dir_mode = 1;
        argi = 2;
    }

    if (argi >= argc) {
        write_fd(1, "Usage: rm [-d] <path>...\n");
        return 1;
    }

    int rc = 0;
    for (int i = argi; i < argc; i++) {
        int ret = dir_mode ? sys_delete_dir(argv[i])
                           : sys_delete_file(argv[i]);
        if (ret != 0) {
            write_fd(1, "rm: cannot delete '");
            write_fd(1, argv[i]);
            write_fd(1, "'\n");
            rc = 1;
        }
    }
    return rc;
}

static int cmd_mkdir(int argc, char **argv)
{
    if (argc < 2) {
        write_fd(1, "Usage: mkdir <path>...\n");
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (sys_mkdir(argv[i], 0777u) != 0) {
            write_fd(1, "mkdir: cannot create '");
            write_fd(1, argv[i]);
            write_fd(1, "'\n");
            rc = 1;
        }
    }
    return rc;
}

/* SIGINT handler: any in-flight applet (cat reading a large log, cp
 * copying a multi-MiB tree, ...) terminates cleanly when the operator
 * presses Ctrl-C, rather than running to completion ignoring the
 * delivered signal. */
static void on_sigint(int signo) { (void)signo; sys_exit(130); }

int main(int argc, char **argv)
{
    sys_signal(SIGINT, on_sigint);

    if (argc < 2) {
        write_fd(1, "Usage: makbox <ls|cat|cp|mv|rm|mkdir|rmdir|echo|pwd> [args...]\n");
        sys_exit(1);
    }

    const char *cmd = argv[1];
    int   sub_argc = argc - 1;
    char **sub_argv = argv + 1;

    int rc = 1;
    if      (streq(cmd, "ls"))   rc = cmd_ls(sub_argc, sub_argv);
    else if (streq(cmd, "cat"))  rc = cmd_cat(sub_argc, sub_argv);
    else if (streq(cmd, "cp"))   rc = cmd_cp(sub_argc, sub_argv);
    else if (streq(cmd, "mv"))   rc = cmd_mv(sub_argc, sub_argv);
    else if (streq(cmd, "rm"))   rc = cmd_rm(sub_argc, sub_argv);
    else if (streq(cmd, "mkdir")) rc = cmd_mkdir(sub_argc, sub_argv);
    else if (streq(cmd, "rmdir")) {
        /* GNU rmdir: each arg is treated as a directory; equivalent to
         * `rm -d` here.  Synthesise the -d flag and reuse cmd_rm. */
        if (sub_argc < 2) { write_fd(1, "Usage: rmdir <dir>...\n"); rc = 1; }
        else {
            static char dflag[] = "-d";
            char *new_argv[18]; /* 16 max args + applet + -d */
            int new_argc = 0;
            new_argv[new_argc++] = sub_argv[0];
            new_argv[new_argc++] = dflag;
            for (int i = 1; i < sub_argc && new_argc < 18; i++)
                new_argv[new_argc++] = sub_argv[i];
            rc = cmd_rm(new_argc, new_argv);
        }
    }
    else if (streq(cmd, "echo")) rc = cmd_echo(sub_argc, sub_argv);
    else if (streq(cmd, "pwd"))  rc = cmd_pwd(sub_argc, sub_argv);
    else {
        write_fd(1, "makbox: unknown applet '");
        write_fd(1, cmd);
        write_fd(1, "'\n");
        write_fd(1, "Usage: makbox <ls|cat|cp|mv|rm|mkdir|rmdir|echo|pwd> [args...]\n");
    }

    sys_exit(rc);
    return rc;
}
