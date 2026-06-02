/*
 * makmux.c -- userspace VT multiplexer scaffold.
 *
 * mak.sh0 starts this explicitly with `makmux`.  The mux process stays
 * alive on the root tty and owns the child login shells mak.sh1..mak.sh4.
 * Each child attaches itself to one of the four kernel VT slots, then execs
 * the normal userspace shell.  Keep this freestanding: in-OS TCC must be
 * able to rebuild it from /src/userspace/makmux.c.
 */

#include "syscall.h"

#define CHILD_COUNT 4

static int s_child_pids[CHILD_COUNT];
static int s_child_focus = 0;

static void put_s(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    sys_write(1, s, n);
}

static void put_dec(int v)
{
    char tmp[12];
    int n = 0;
    if (v < 0) {
        char m = '-';
        sys_write(1, &m, 1);
        v = -v;
    }
    if (v == 0) tmp[n++] = '0';
    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        char c = tmp[--n];
        sys_write(1, &c, 1);
    }
}

static void child_shell(void)
{
    int slot = sys_vt_enter(s_child_focus);
    if (slot < 0) {
        put_s("makmux: no VT slot available for child shell\n");
        sys_exit(1);
    }

    char *argv[] = { "sh.elf", "--makmux", 0 };
    sys_execve("/apps/sh.elf", argv, (char *const *)0);
    put_s("makmux: exec /apps/sh.elf failed on VT ");
    put_dec(slot);
    put_s("\n");
    sys_exit(127);
}

static int spawn_child(int idx, int focus)
{
    s_child_focus = focus;
    int pid = sys_fork();
    if (pid < 0) return -1;
    if (pid == 0) child_shell();
    s_child_focus = 0;
    s_child_pids[idx] = pid;
    return 0;
}

static int live_children(void)
{
    int n = 0;
    for (int i = 0; i < CHILD_COUNT; i++)
        if (s_child_pids[i] > 0) n++;
    return n;
}

static int first_free_child_slot(void)
{
    for (int i = 0; i < CHILD_COUNT; i++)
        if (s_child_pids[i] <= 0) return i;
    return -1;
}

static int slen(const char *s)
{
    int n = 0;
    while (s && s[n]) n++;
    return n;
}


static void note_child_exit(int pid)
{
    for (int i = 0; i < CHILD_COUNT; i++) {
        if (s_child_pids[i] == pid) {
            s_child_pids[i] = 0;
            return;
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Refuse to nest: if VT slots are already registered another makmux
     * (or a nested exec of makmux from within mak.sh1-4) is already
     * running.  live_mask != 0 is the indicator. */
    if (sys_vt_state() & 0xFFFF) {
        put_s("makmux: already running -- cannot nest makmux\n");
        return 1;
    }

    put_s("makmux: starting mak.sh1-4\n");

    int ok = 1;
    for (int i = 0; i < CHILD_COUNT; i++) {
        if (spawn_child(i, 0) != 0) {
            put_s("makmux: fork failed for mak.sh");
            put_dec(i + 1);
            put_s("\n");
            ok = 0;
        }
    }

    put_s("makmux: use Alt+F1..Alt+F4 for mak.sh1..mak.sh4\n");
    if (!ok) put_s("makmux: one or more child shells failed to start\n");

    /* makmux now only manages the VT children.  The bottom status bar
     * (hostname + VT tabs + clock) is drawn by the kernel statusbar task,
     * which reads the live VT state -- so Alt+F5 (bar toggle) and the clock
     * are handled there, not here. */
    for (;;) {
        int status = 0;
        int pid = sys_wait4(-1, &status, WNOHANG);
        if (pid > 0) {
            put_s("makmux: child shell exited pid=");
            put_dec(pid);
            put_s(" status=");
            put_dec(status);
            put_s("\n");
            sys_vt_close(pid);
            note_child_exit(pid);
            if (live_children() == 0)
                return 0;
        }

        int opens = sys_vt_open_request();
        while (opens-- > 0 && live_children() < CHILD_COUNT) {
            int idx = first_free_child_slot();
            if (idx >= 0)
                spawn_child(idx, 1);
        }

        sys_yield();
    }
}
