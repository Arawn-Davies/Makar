/*
 * filetest.elf -- userspace verifier for the file create/write/stat path
 * shipped in the Phase-1 PR that prepares Makar for the TCC port.
 *
 * Drives every behaviour the new syscall surface introduces:
 *   1.  open(O_WRONLY|O_CREAT|O_TRUNC) + write + close            (create + flush)
 *   2.  open(O_RDONLY) + fstat + read + memcmp                     (readback + fstat)
 *   3.  open(O_WRONLY|O_APPEND) + write + close                    (O_APPEND)
 *   4.  stat() reflects the appended size                          (SYS_STAT)
 *   5.  256 KiB write past the old SYSCALL_FILE_MAX cap            (growable buffer)
 *   6.  open(O_RDONLY) + close (no writes) leaves file unchanged   (no-flush-on-rdonly)
 *
 * Prints PASS/FAIL lines over COM1 (SYS_WRITE_SERIAL) and exits non-zero
 * on the first failure so the in-guest test drivers can gate on $?.  Final
 * status: "[filetest] PASS" on full success, "[filetest] FAIL: <reason>"
 * on the first failure.
 *
 * Argv[1] (optional) selects the writable directory to use.  Default is
 * /mnt/scratch.  The big-file path
 * is "<dir>/filetest.big"; the main scratch is "<dir>/filetest.tmp".
 */

#include "syscall.h"

static int my_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* Mirror every status line to BOTH serial (for the in-guest test drivers) and
 * stdout (so a human running the .elf interactively sees the progress). */
static void srl(const char *s)
{
    int n = my_strlen(s);
    syscall2(SYS_WRITE_SERIAL, (long)s, (long)n);
    syscall3(SYS_WRITE, 1, (long)s, (long)n);
}

static void srl_u(unsigned int v)
{
    /* decimal -- small helper, no libc */
    char buf[12]; int i = 11; buf[i--] = '\0';
    if (v == 0) { buf[i--] = '0'; }
    while (v) { buf[i--] = (char)('0' + (v % 10u)); v /= 10u; }
    srl(&buf[i + 1]);
}

static int memequ(const unsigned char *a, const unsigned char *b, unsigned int n)
{
    for (unsigned int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Build "<dir>/<name>" in `out`.  Caller guarantees enough space. */
static void join_path(char *out, const char *dir, const char *name)
{
    int i = 0;
    while (*dir) out[i++] = *dir++;
    if (i > 0 && out[i - 1] != '/') out[i++] = '/';
    while (*name) out[i++] = *name++;
    out[i] = '\0';
}

static int fail(const char *why) { srl("[filetest] FAIL: "); srl(why); srl("\n"); return 1; }

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)envp;

    const char *dir = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : "/mnt/scratch";
    char tmp_path[160], big_path[160], ro_path[160];
    join_path(tmp_path, dir, "filetest.tmp");
    join_path(big_path, dir, "filetest.big");
    join_path(ro_path,  dir, "filetest.ro");

    srl("[filetest] dir=");
    srl(dir);
    srl("\n");

    /* ---- 1. create + write + close ---- */
    int fd = sys_open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return fail("open(create) failed");
    const char *msg1 = "hello, world\n";   /* 13 bytes */
    long w = syscall3(SYS_WRITE, (long)fd, (long)msg1, 13);
    if (w != 13) return fail("write#1 short");
    if (sys_close(fd) != 0) return fail("close#1 (flush) failed");
    srl("[filetest] create+write+close ok\n");

    /* ---- 2. reopen + fstat + read + verify ---- */
    fd = sys_open(tmp_path, O_RDONLY);
    if (fd < 0) return fail("reopen(rdonly) failed");
    struct stat st;
    if (sys_fstat(fd, &st) != 0) return fail("fstat#1 failed");
    if (st.st_size != 13) return fail("fstat#1 size != 13");
    if (!S_ISREG(st.st_mode)) return fail("fstat#1 mode not REG");
    char rb[32];
    long r = syscall3(SYS_READ, (long)fd, (long)rb, 32);
    if (r != 13) return fail("read#1 short");
    if (!memequ((unsigned char *)rb, (unsigned char *)msg1, 13))
        return fail("read#1 content mismatch");
    sys_close(fd);
    srl("[filetest] reopen+fstat+read ok size=");
    srl_u(st.st_size); srl("\n");

    /* ---- 3. O_APPEND ---- */
    fd = sys_open(tmp_path, O_WRONLY | O_APPEND);
    if (fd < 0) return fail("open(append) failed");
    const char *msg2 = " more\n";   /* 6 bytes -> total 19 */
    w = syscall3(SYS_WRITE, (long)fd, (long)msg2, 6);
    if (w != 6) return fail("write#2 short");
    if (sys_close(fd) != 0) return fail("close#2 (flush append) failed");
    srl("[filetest] append+close ok\n");

    /* ---- 4. sys_stat reflects appended size ---- */
    if (sys_stat(tmp_path, &st) != 0) return fail("stat failed");
    if (st.st_size != 19) return fail("stat size != 19");
    srl("[filetest] stat ok size="); srl_u(st.st_size); srl("\n");

    /* ---- 5. 256 KiB grow-past-old-cap ---- */
    fd = sys_open(big_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return fail("open(big) failed");
    /* Write 4 KiB chunks of a deterministic byte pattern.  Total 256 KiB,
     * which used to be 4x the old SYSCALL_FILE_MAX. */
    static unsigned char chunk[4096];
    const unsigned int total = 256u * 1024u;
    for (unsigned int off = 0; off < total; off += 4096u) {
        for (unsigned int i = 0; i < 4096; i++)
            chunk[i] = (unsigned char)((off + i) & 0xFFu);
        w = syscall3(SYS_WRITE, (long)fd, (long)chunk, 4096);
        if (w != 4096) return fail("big-write short");
    }
    if (sys_close(fd) != 0) return fail("big-close (flush) failed");
    /* Verify size via stat, then read back and verify pattern. */
    if (sys_stat(big_path, &st) != 0) return fail("big-stat failed");
    if (st.st_size != total) return fail("big-size mismatch");
    fd = sys_open(big_path, O_RDONLY);
    if (fd < 0) return fail("big-reopen failed");
    unsigned int verified = 0;
    while (verified < total) {
        long got = syscall3(SYS_READ, (long)fd, (long)chunk, 4096);
        if (got <= 0) return fail("big-read short");
        for (long i = 0; i < got; i++) {
            if (chunk[i] != (unsigned char)((verified + (unsigned int)i) & 0xFFu))
                return fail("big-readback pattern mismatch");
        }
        verified += (unsigned int)got;
    }
    sys_close(fd);
    srl("[filetest] grow-256k ok\n");

    /* ---- 6. no-flush on read-only close: pre-populate via SYS_WRITE_FILE,
     *        then open(O_RDONLY), close immediately, verify content
     *        unchanged. ---- */
    const char *ro_content = "untouched\n";
    /* SYS_WRITE_FILE creates/overwrites; matches the old whole-buffer path. */
    if ((int)syscall3(SYS_WRITE_FILE, (long)ro_path,
                      (long)ro_content, 10) != 0)
        return fail("ro-precreate failed");
    fd = sys_open(ro_path, O_RDONLY);
    if (fd < 0) return fail("ro-open failed");
    if (sys_close(fd) != 0) return fail("ro-close failed");
    /* Now re-open and compare content. */
    fd = sys_open(ro_path, O_RDONLY);
    if (fd < 0) return fail("ro-reopen failed");
    char rcheck[16];
    long got = syscall3(SYS_READ, (long)fd, (long)rcheck, 16);
    sys_close(fd);
    if (got != 10 || !memequ((unsigned char *)rcheck,
                             (unsigned char *)ro_content, 10))
        return fail("ro-content drifted");
    srl("[filetest] no-flush-on-rdonly ok\n");

    /* ---- cleanup (best-effort) ---- */
    syscall1(SYS_DELETE_FILE, (long)tmp_path);
    syscall1(SYS_DELETE_FILE, (long)big_path);
    syscall1(SYS_DELETE_FILE, (long)ro_path);

    srl("[filetest] PASS\n");
    return 0;
}
