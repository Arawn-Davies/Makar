/* ktest_uspace.c - userspace counterpart to ktest_bg.
 *
 * Covers the four suites removed from ktest_bg that test visible syscall
 * behaviour rather than kernel internals:
 *
 *   USR     - /usr sysroot content reachable (crt0.o, libc.a, stdio.h)
 *   TMPFS   - /tmp write / read / stat / unlink via SYS_* syscalls
 *   FD      - write, lseek, read-back via fd syscalls
 *   CWD     - SYS_GETCWD / SYS_CHDIR round-trip
 *
 * Exit 0 = all pass, 1 = any failure.  incore.sh runs this and checks $?.
 * No HMP, no sendkey, no serial grep.
 */
#include "syscall.h"

/* ------------------------------------------------------------------ */
/* Minimal helpers                                                      */
/* ------------------------------------------------------------------ */

typedef unsigned int   u32;
typedef unsigned long  usize;

static void put_s(const char *s)
{
    usize n = 0; while (s[n]) n++;
    sys_write(1, s, n);
}

static void put_u(u32 v)
{
    char buf[12]; int n = 0;
    if (v == 0) { sys_write(1, "0", 1); return; }
    while (v) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = n-1; i >= 0; i--) sys_write(1, &buf[i], 1);
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int pass_count = 0;
static int fail_count = 0;

#define ASSERT(cond, label) do { \
    if (cond) { pass_count++; } \
    else { put_s("[FAIL] " label "\n"); fail_count++; } \
} while (0)

/* ------------------------------------------------------------------ */
/* Suite: USR - /usr sysroot content                                   */
/* ------------------------------------------------------------------ */

static int file_exists(const char *path)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return 0;
    sys_close(fd);
    return 1;
}

static void suite_usr(void)
{
    put_s("[ktest_uspace] suite: usr\n");

    /* Skip entirely if sysroot isn't populated (no /usr on this rootfs). */
    if (!file_exists("/usr/lib/crt0.o")) {
        put_s("[ktest_uspace] usr: no sysroot, skipping\n");
        return;
    }

    ASSERT(file_exists("/usr/lib/crt0.o"),      "/usr/lib/crt0.o exists");
    ASSERT(file_exists("/usr/lib/libc.a"),       "/usr/lib/libc.a exists");
    ASSERT(file_exists("/usr/lib/crt1.o"),       "/usr/lib/crt1.o exists");
    ASSERT(file_exists("/usr/lib/crti.o"),       "/usr/lib/crti.o exists");
    ASSERT(file_exists("/usr/lib/crtn.o"),       "/usr/lib/crtn.o exists");
    ASSERT(file_exists("/usr/lib/tcc/libtcc1.a"), "/usr/lib/tcc/libtcc1.a exists");
    ASSERT(file_exists("/usr/include/stdio.h"),  "/usr/include/stdio.h exists");
    ASSERT(file_exists("/usr/include/string.h"), "/usr/include/string.h exists");
    ASSERT(file_exists("/apps/hello.elf"),       "/apps/hello.elf exists");
}

/* ------------------------------------------------------------------ */
/* Suite: TMPFS - /tmp via syscalls                                    */
/* ------------------------------------------------------------------ */

static void suite_tmpfs(void)
{
    put_s("[ktest_uspace] suite: tmpfs\n");

    const char *path = "/tmp/ktest_uspace_probe.bin";
    const char *body = "ktest_uspace_tmpfs_body";
    usize blen = 23;

    /* Write */
    {
        int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
        ASSERT(fd >= 0, "open /tmp for write");
        if (fd >= 0) {
            long n = sys_write(fd, body, blen);
            ASSERT((usize)n == blen, "write byte count");
            sys_close(fd);
        }
    }

    ASSERT(file_exists(path), "file exists after write");

    /* Read back */
    {
        char buf[64];
        int fd = sys_open(path, O_RDONLY);
        ASSERT(fd >= 0, "open /tmp for read");
        if (fd >= 0) {
            long n = sys_read(fd, buf, sizeof(buf) - 1);
            ASSERT(n == (long)blen, "read byte count");
            if (n > 0) {
                buf[n] = '\0';
                ASSERT(str_eq(buf, body), "read-back content matches");
            }
            sys_close(fd);
        }
    }

    /* SYS_UNLINK */
    {
        int r = sys_unlink(path);
        ASSERT(r == 0, "unlink returns 0");
        ASSERT(!file_exists(path), "file gone after unlink");
    }
}

/* ------------------------------------------------------------------ */
/* Suite: FD - write / lseek / read via fd                             */
/* ------------------------------------------------------------------ */

static void suite_fd(void)
{
    put_s("[ktest_uspace] suite: fd\n");

    const char *path = "/tmp/ktest_uspace_fd.bin";

    /* Write 'A' at offset 0, seek to 100, write 'Z', read back. */
    int fd = sys_open(path, O_RDWR | O_CREAT | O_TRUNC);
    ASSERT(fd >= 0, "open fd test file");
    if (fd < 0) return;

    long n = sys_write(fd, "A", 1);
    ASSERT(n == 1, "write A");

    long pos = sys_lseek(fd, 100, 0);  /* SEEK_SET */
    ASSERT(pos == 100, "lseek to 100");

    n = sys_write(fd, "Z", 1);
    ASSERT(n == 1, "write Z at 100");

    /* Seek back to 0 and read 101 bytes. */
    sys_lseek(fd, 0, 0);
    char buf[128];
    n = sys_read(fd, buf, 101);
    ASSERT(n == 101, "read 101 bytes");
    if (n == 101) {
        ASSERT(buf[0]   == 'A', "byte[0] == A");
        ASSERT(buf[100] == 'Z', "byte[100] == Z");
        int gap_ok = 1;
        for (int i = 1; i < 100; i++) if (buf[i] != 0) { gap_ok = 0; break; }
        ASSERT(gap_ok, "gap bytes are zero");
    }

    sys_close(fd);
    sys_unlink(path);

    /* SYS_STAT on /proc/uname */
    {
        struct stat st;
        int r = sys_stat("/proc/uname", &st);
        ASSERT(r == 0, "stat /proc/uname");
        ASSERT((st.st_mode & S_IFMT) == S_IFREG, "/proc/uname is regular file");
        ASSERT(st.st_size > 0, "/proc/uname size > 0");
    }

    /* SYS_STAT on nonexistent path */
    {
        struct stat st;
        int r = sys_stat("/no/such/path/ktest_uspace", &st);
        ASSERT(r < 0, "stat missing path returns error");
    }
}

/* ------------------------------------------------------------------ */
/* Suite: CWD - getcwd / chdir round-trip                              */
/* ------------------------------------------------------------------ */

static void suite_cwd(void)
{
    put_s("[ktest_uspace] suite: cwd\n");

    char saved[256];
    long n = sys_getcwd(saved, sizeof(saved));
    ASSERT(n > 0, "getcwd returns > 0");

    /* cd to /tmp */
    int r = sys_chdir("/tmp");
    ASSERT(r == 0, "chdir /tmp");

    char cur[256];
    n = sys_getcwd(cur, sizeof(cur));
    ASSERT(n > 0, "getcwd after chdir");
    ASSERT(cur[0] == '/' && cur[1] == 't' && cur[2] == 'm' && cur[3] == 'p',
           "cwd is /tmp after chdir");

    /* Restore */
    r = sys_chdir(saved);
    ASSERT(r == 0, "chdir back to saved");

    n = sys_getcwd(cur, sizeof(cur));
    int same = 1;
    for (int i = 0; saved[i] || cur[i]; i++)
        if (saved[i] != cur[i]) { same = 0; break; }
    ASSERT(same, "cwd restored to original");
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    put_s("[ktest_uspace] BEGIN\n");

    suite_usr();
    suite_tmpfs();
    suite_fd();
    suite_cwd();

    put_s("[ktest_uspace] pass="); put_u((u32)pass_count);
    put_s(" fail="); put_u((u32)fail_count); put_s("\n");

    if (fail_count == 0) {
        put_s("[ktest_uspace] ALL PASS\n");
        return 0;
    }
    put_s("[ktest_uspace] FAILED\n");
    return 1;
}
