/*
 * tcc_compat.c -- POSIX-flavoured libc surface needed by TCC.
 *
 * TCC's source references a set of "hosted" libc functions that the
 * Makar shim doesn't otherwise need (open/close/read/write at the
 * POSIX wrapper layer, sprintf, strtoull, fseek/ftell, exit, ...).
 * Each one here is a thin adapter onto the existing Makar syscall
 * surface or onto another shim function.
 *
 * The objects ship in libc.a so other apps stay un-affected: anything
 * they don't reference, the linker discards.
 */

#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "errno.h"

/* Local helper: set errno from a Makar -1 return and translate the
 * common cases.  Kernel-side dispatch returns plain -1 today (not a
 * negative errno), so the best we can do is map by syscall context.
 * Each wrapper passes the errno value to use when the call failed. */
static int set_err_if_neg(long ret, int err)
{
    if (ret < 0) { errno = err; return -1; }
    return (int)ret;
}

/* ---- POSIX file I/O wrappers ---------------------------------------- */

int open(const char *path, int flags, ...)
{
    return sys_open(path, flags);
}

int close(int fd)
{
    return sys_close(fd);
}

long read(int fd, void *buf, unsigned n)
{
    return sys_read(fd, buf, n);
}

long write(int fd, const void *buf, unsigned n)
{
    return sys_write(fd, buf, n);
}

long lseek(int fd, long offset, int whence)
{
    return sys_lseek(fd, (int)offset, whence);
}

int unlink(const char *path)
{
    return set_err_if_neg(sys_unlink(path), ENOENT);
}

int rmdir(const char *path)
{
    return set_err_if_neg(sys_rmdir(path), ENOENT);
}

int rename(const char *old_path, const char *new_path)
{
    return set_err_if_neg(sys_rename(old_path, new_path), ENOENT);
}

int mkdir(const char *path, unsigned int mode)
{
    return set_err_if_neg(sys_mkdir(path, mode), EEXIST);
}

int remove(const char *path)
{
    /* POSIX: try unlink first, fall back to rmdir for directories. */
    if (sys_unlink(path) == 0) return 0;
    return set_err_if_neg(sys_rmdir(path), ENOENT);
}

int chmod(const char *path, unsigned int mode)
{
    (void)path; (void)mode; return 0;   /* Makar has no permission bits today */
}

/* access(2): F_OK/R_OK/W_OK/X_OK -- without a permission model, the only
 * thing we can answer is "does the path exist?" via stat. */
int access(const char *path, int mode)
{
    (void)mode;
    struct stat st;
    return set_err_if_neg(sys_stat(path, &st), ENOENT);
}

int stat(const char *path, struct stat *st)
{
    return sys_stat(path, st);
}

int fstat(int fd, struct stat *st)
{
    return sys_fstat(fd, st);
}

/* ---- Process control ------------------------------------------------ */

__attribute__((noreturn))
void exit(int status)
{
    sys_exit(status);
    while (1) {}   /* sys_exit doesn't return; satisfy noreturn nonetheless */
}

__attribute__((noreturn))
void abort(void)
{
    sys_exit(134);                  /* 128 + SIGABRT */
    while (1) {}
}

/* Hardcoded PATH -- Makar has no envp plumbing yet.  Mirrors the
 * default in sh.elf's shell_path_dir() (which falls back to "/apps"
 * when $PATH is unset).  /bin is included for forward compatibility
 * once /usr/local/bin etc. land. */
static const char *kExecvpPath = "/apps:/bin";

int execvp(const char *file, char *const argv[])
{
    if (!file || !*file) { errno = ENOENT; return -1; }

    /* If the name contains a `/`, it's a path -- no PATH walk. */
    for (const char *p = file; *p; p++) {
        if (*p == '/') return sys_execve(file, argv, 0);
    }

    char buf[256];
    const char *path = kExecvpPath;
    int last_err = ENOENT;
    while (*path) {
        const char *seg = path;
        while (*path && *path != ':') path++;
        unsigned int slen = (unsigned int)(path - seg);
        if (*path == ':') path++;
        if (slen == 0) continue;

        unsigned int fl = 0;
        while (file[fl]) fl++;
        if (slen + 1 + fl + 1 > sizeof(buf)) { last_err = EINVAL; continue; }

        unsigned int j = 0;
        for (unsigned int i = 0; i < slen; i++) buf[j++] = seg[i];
        if (buf[j - 1] != '/') buf[j++] = '/';
        for (unsigned int i = 0; i < fl; i++) buf[j++] = file[i];
        buf[j] = '\0';

        struct stat st;
        if (sys_stat(buf, &st) != 0) continue;
        sys_execve(buf, argv, 0);
        /* Only reached if execve failed -- record and keep walking. */
        last_err = EACCES;
    }
    errno = last_err;
    return -1;
}

/* ---- 64-bit strtol/strtoul ------------------------------------------ */

long long strtoll(const char *s, char **endp, int base)
{
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    long long acc = 0;
    int any = 0;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2; base = 16;
    } else if (base == 0 && *p == '0') {
        /* Octal prefix: consume the '0' AND count it as a digit, so a bare
         * "0" parses as the value 0 with endp past it (matches glibc).
         * Without `any = 1` here, the digit loop below sees nothing more
         * and the function returns endp=s, which breaks TCC's asm parser
         * for any literal `0` (e.g. `.byte 0`). */
        p++; base = 8; any = 1;
    } else if (base == 0) base = 10;
    while (*p) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= base) break;
        acc = acc * (long long)base + (long long)d;
        any = 1; p++;
    }
    if (endp) *endp = (char *)(any ? p : s);
    return neg ? -acc : acc;
}

unsigned long long strtoull(const char *s, char **endp, int base)
{
    return (unsigned long long)strtoll(s, endp, base);
}

/* ---- sprintf via vsnprintf into a generous scratch ------------------ */

int sprintf(char *buf, const char *fmt, ...)
{
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    /* No upper bound is "right" here.  4 KiB is the working ceiling for
     * the formatted strings TCC produces (relocation names, file paths). */
    int n = vsnprintf(buf, 4096u, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

int vsprintf(char *buf, const char *fmt, __builtin_va_list ap)
{
    return vsnprintf(buf, 4096u, fmt, ap);
}

/* ---- stdio extras ---------------------------------------------------- */

int fseek(FILE *f, long offset, int whence)
{
    if (!f) return -1;
    fflush(f);
    return (sys_lseek(f->fd, (int)offset, whence) < 0) ? -1 : 0;
}

long ftell(FILE *f)
{
    if (!f) return -1;
    fflush(f);
    return sys_lseek(f->fd, 0, SEEK_CUR);
}

FILE *fdopen(int fd, const char *mode)
{
    (void)mode;
    /* Allocate a FILE* over an already-open fd.  TCC uses this once,
     * inside tcc_write_elf_file, to wrap the output fd before
     * fwrite'ing the ELF bytes into it. */
    extern void *malloc(unsigned int);
    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) return 0;
    f->fd = fd; f->err = 0; f->eof = 0; f->wlen = 0;
    return f;
}

/* fprintf to stderr is the typical TCC diagnostic path; vfprintf
 * fills the same role with an explicit va_list. */
int vfprintf(FILE *f, const char *fmt, __builtin_va_list ap)
{
    char scratch[2048];
    int n = vsnprintf(scratch, sizeof(scratch), fmt, ap);
    fwrite(scratch, 1, (unsigned int)n, f);
    return n;
}

/* ---- time / random -------------------------------------------------- */

unsigned int time(unsigned int *t)
{
    /* POSIX time(2): seconds since 1970-01-01 UTC, from the CMOS RTC. */
    struct timeval tv;
    unsigned int now = 0;
    if (sys_gettimeofday(&tv) == 0) now = (unsigned int)tv.tv_sec;
    if (t) *t = now;
    return now;
}

struct tm_stub { int a[9]; };

struct tm_stub *localtime(const unsigned int *t)
{
    (void)t;
    static struct tm_stub zero;
    return &zero;
}

int gettimeofday(void *tv, void *tz)
{
    (void)tz;
    if (!tv) return 0;
    return set_err_if_neg(sys_gettimeofday((struct timeval *)tv), EFAULT);
}

int clock_gettime(int clk, struct timespec *ts)
{
    return set_err_if_neg(sys_clock_gettime(clk, ts), EINVAL);
}

/* ---- errno ---------------------------------------------------------- */

int errno = 0;

/* ---- assert --------------------------------------------------------- */

void __assert_fail(const char *expr, const char *file, int line, const char *func)
{
    (void)expr; (void)file; (void)line; (void)func;
    sys_write(2, "assertion failed\n", 17);
    sys_exit(134);
}

/* ---- mmap stub (tccrun.c JIT path; never actually invoked) ---------- */

void *mmap(void *a, unsigned int sz, int prot, int fl, int fd, long off)
{
    (void)a; (void)sz; (void)prot; (void)fl; (void)fd; (void)off;
    return (void *)-1;     /* MAP_FAILED */
}

int munmap(void *a, unsigned int sz)
{
    (void)a; (void)sz; return -1;
}

/* ---- math placeholders (unused float-folding paths) ----------------- */

double ldexp(double x, int e)         { (void)e; return x; }
double frexp(double x, int *e)        { if (e) *e = 0; return x; }

/* TCC's parse_number references strtod/strtof/strtold via the
 * floating-point constant lexer.  Programs that consume floating
 * literals will fail to fold them, but for the bring-up cases
 * (integer-only sources) the symbols just need to resolve. */
float       strtof (const char *s, char **e) { (void)s; if (e) *e = (char *)s; return 0.0f; }
double      strtod (const char *s, char **e) { (void)s; if (e) *e = (char *)s; return 0.0;  }
long double strtold(const char *s, char **e) { (void)s; if (e) *e = (char *)s; return 0.0L; }

/* getcwd: TCC uses it once for debug-info emission. */
char *getcwd(char *buf, unsigned int size)
{
    if (sys_getcwd(buf, size) < 0) return 0;
    return buf;
}

/* mprotect: tccrun.c's JIT page-permission flip; unreachable on Makar
 * (we don't ship -run) but the symbol must resolve. */
int mprotect(void *addr, unsigned int len, int prot)
{
    (void)addr; (void)len; (void)prot; return -1;
}
