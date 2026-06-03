/*
 * alloctest.elf -- exercises the userspace heap (malloc.c) and the new
 * stdlib helpers (strtol/atoi) + ctype classifiers shipped alongside the
 * Phase-1 TCC-port slice.  PASS/FAIL summary goes to COM1 and the exit
 * status gates the incore.sh check -- no host input, no serial grep.
 *
 * Coverage:
 *   1. brk-backed malloc: allocate, write, read-back, free
 *   2. free + realloc: free pattern, then fresh malloc reuses freed bytes
 *   3. realloc grow (in place when possible; copy otherwise)
 *   4. calloc zeroes
 *   5. ctype classifiers behave per POSIX on a spot-check vector
 *   6. strtol parses decimal, hex (0x), octal (0), negative, with endp
 *   7. atoi matches strtol's decimal path
 *   8-12. realloc/calloc, file I/O, FILE* stdio, readdir, strdup/qsort/sscanf
 *   13. writable environment: setenv / getenv / unsetenv / putenv
 *   14. strtok_r (consecutive-delimiter skipping) + strerror table
 *   15. bsearch over a sorted array
 *   16. pipe + dup roundtrip (single process)
 *   17. time.h: gmtime_r / mktime roundtrip / strftime subset
 *   18. system(): shell availability + exit-status round-trip
 */

#include "syscall.h"
#include "ctype.h"
#include "stdlib.h"
#include "setjmp.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "time.h"
#include "errno.h"

static int my_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
/* Mirror every status line to BOTH serial (for the in-guest test drivers) and
 * stdout (so a human running `exec alloctest.elf` interactively sees
 * the progress on screen instead of staring at a blinking cursor). */
static void srl(const char *s)
{
    int n = my_strlen(s);
    syscall2(SYS_WRITE_SERIAL, (long)s, (long)n);
    syscall3(SYS_WRITE, 1, (long)s, (long)n);
}
static int fail(const char *why) { srl("[alloctest] FAIL: "); srl(why); srl("\n"); return 1; }

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    /* ---- 1. small malloc + write + free ---- */
    char *a = (char *)malloc(64);
    if (!a) return fail("malloc(64) returned NULL");
    for (int i = 0; i < 64; i++) a[i] = (char)(i ^ 0xA5);
    for (int i = 0; i < 64; i++)
        if (a[i] != (char)(i ^ 0xA5)) return fail("malloc#1 content drift");
    free(a);
    srl("[alloctest] malloc/free 64B ok\n");

    /* ---- 2. free + reuse ---- */
    char *b = (char *)malloc(64);
    char *c = (char *)malloc(64);
    if (!b || !c) return fail("malloc reuse alloc");
    free(b);
    char *d = (char *)malloc(48);   /* should fit in b's slot */
    if (!d) return fail("realloc-of-free");
    /* d and c must not alias. */
    if (d == c) return fail("d aliases c");
    free(c);
    free(d);
    srl("[alloctest] reuse ok\n");

    /* ---- 3. realloc grow ---- */
    char *r = (char *)malloc(16);
    if (!r) return fail("realloc precursor");
    for (int i = 0; i < 16; i++) r[i] = (char)('A' + i);
    char *r2 = (char *)realloc(r, 1024);
    if (!r2) return fail("realloc grow");
    for (int i = 0; i < 16; i++)
        if (r2[i] != (char)('A' + i)) return fail("realloc content");
    free(r2);
    srl("[alloctest] realloc grow ok\n");

    /* ---- 4. calloc zeroes ---- */
    int *z = (int *)calloc(32, sizeof(int));
    if (!z) return fail("calloc");
    for (int i = 0; i < 32; i++)
        if (z[i] != 0) return fail("calloc not zero");
    free(z);
    srl("[alloctest] calloc zeroes ok\n");

    /* ---- 5. ctype spot-check ---- */
    if (!isdigit('7') || isdigit('a'))   return fail("isdigit");
    if (!isalpha('z') || isalpha('5'))   return fail("isalpha");
    if (!isalnum('Q') || isalnum('!'))   return fail("isalnum");
    if (!isspace(' ') || isspace('x'))   return fail("isspace");
    if (!isxdigit('f') || isxdigit('g')) return fail("isxdigit");
    if (toupper('a') != 'A')             return fail("toupper");
    if (tolower('Z') != 'z')             return fail("tolower");
    srl("[alloctest] ctype ok\n");

    /* ---- 6. strtol ---- */
    char *end = 0;
    if (strtol("  42abc", &end, 10) != 42)     return fail("strtol dec");
    if (end == 0 || *end != 'a')               return fail("strtol endp");
    if (strtol("0x2A", 0, 0) != 42)            return fail("strtol hex auto");
    if (strtol("2A", 0, 16) != 42)             return fail("strtol hex explicit");
    if (strtol("052", 0, 0) != 42)             return fail("strtol oct auto");
    if (strtol("-17", 0, 10) != -17)           return fail("strtol neg");
    /* no-conversion: endp == s */
    long x = strtol("nope", &end, 10);
    (void)x;
    if (end == 0)                              return fail("strtol no-conv endp");
    srl("[alloctest] strtol ok\n");

    /* ---- 7. atoi ---- */
    if (atoi("123") != 123) return fail("atoi");
    if (atoi("-9")  != -9)  return fail("atoi neg");
    srl("[alloctest] atoi ok\n");

    /* ---- 8. setjmp / longjmp ---- */
    {
        static jmp_buf env;
        volatile int hit = 0;
        int rv = setjmp(env);
        if (rv == 0) {
            hit = 1;
            longjmp(env, 42);            /* never returns */
            return fail("longjmp returned");
        }
        if (rv != 42) return fail("setjmp returned wrong val");
        if (!hit)     return fail("setjmp skipped pre-longjmp code");

        /* longjmp(env, 0) must surface as setjmp returning 1 (POSIX). */
        rv = setjmp(env);
        if (rv == 0) { longjmp(env, 0); return fail("longjmp(0) returned"); }
        if (rv != 1) return fail("setjmp/longjmp(0) != 1");
    }
    srl("[alloctest] setjmp/longjmp ok\n");

    /* ---- 9. snprintf ---- */
    {
        char b[64];
        int n = snprintf(b, sizeof(b), "x=%d hex=0x%04x s=%s c=%c", -7, 0xAB, "hi", '!');
        if (n <= 0) return fail("snprintf returned <=0");
        /* expect: "x=-7 hex=0x00ab s=hi c=!" */
        const char *want = "x=-7 hex=0x00ab s=hi c=!";
        for (int i = 0; want[i]; i++)
            if (b[i] != want[i]) return fail("snprintf content mismatch");
        /* truncation guard: buf small enough to clip */
        int m = snprintf(b, 6, "hello world");
        if (m != 11) return fail("snprintf return != full length");
        if (b[5] != '\0') return fail("snprintf missing NUL");
    }
    srl("[alloctest] snprintf ok\n");

    /* ---- 10. FILE* layer (fopen/fwrite/fclose -> fopen/fread/fclose) ---- */
    {
        /* /tmp -- tmpfs_write replaces the whole file each call, matching
         * POSIX-style O_TRUNC semantics so re-runs see the freshly-written
         * 16 bytes.  /log was wrong: logfs_write is a ring-append (dmesg
         * pattern), so re-running this test appended 16+16=32 bytes and
         * the fread cap of 32 returned 32 -> "fread short". */
        const char *path = "/tmp/alloctest.tmp";
        FILE *f = fopen(path, "w");
        if (!f) return fail("fopen w");
        const char *msg = "alloc-file-line\n";
        unsigned int w = fwrite(msg, 1, 16, f);
        if (w != 16) return fail("fwrite short");
        if (fclose(f) != 0) return fail("fclose w");
        f = fopen(path, "r");
        if (!f) return fail("fopen r");
        char rb[32]; for (int i = 0; i < 32; i++) rb[i] = 0;
        unsigned int r = fread(rb, 1, 32, f);
        if (r != 16) return fail("fread short");
        for (int i = 0; i < 16; i++)
            if (rb[i] != msg[i]) return fail("FILE roundtrip mismatch");
        fclose(f);
    }
    srl("[alloctest] FILE* roundtrip ok\n");

    /* ---- 11. SYS_READDIR over /tmp (we just created alloctest.tmp) ---- */
    {
        struct dirent de;
        int found = 0;
        for (unsigned int i = 0; i < 32; i++) {
            int rc = sys_readdir("/tmp", i, &de);
            if (rc <= 0) break;
            /* /log entries are file names (no leading slash, no dir). */
            if (de.d_name[0] == 'a' && de.d_name[1] == 'l' &&
                de.d_name[2] == 'l' && de.d_name[3] == 'o')
                found = 1;
        }
        if (!found) return fail("readdir missing alloctest.tmp");
    }
    srl("[alloctest] readdir ok\n");

    /* ---- 12. strdup / qsort / sscanf / getenv ---- */
    {
        char *dup = strdup("hello");
        if (!dup) return fail("strdup OOM");
        if (dup[0] != 'h' || dup[4] != 'o' || dup[5] != '\0') return fail("strdup content");
        free(dup);

        int arr[] = { 5, 2, 9, 1, 7, 3 };
        qsort(arr, 6, sizeof(int), cmp_int);
        for (int i = 0; i < 5; i++) if (arr[i] > arr[i + 1]) return fail("qsort");

        int x = 0, y = 0; char word[16] = {0};
        int n = sscanf("42 -7 banana", "%d %d %s", &x, &y, word);
        if (n != 3) return fail("sscanf nmatched");
        if (x != 42 || y != -7) return fail("sscanf ints");
        if (word[0] != 'b' || word[5] != 'a' || word[6] != '\0') return fail("sscanf string");

        if (getenv("PATH") != 0) return fail("getenv should miss");
    }
    srl("[alloctest] strdup/qsort/sscanf/getenv ok\n");

    /* ---- 13. writable environment: setenv / getenv / unsetenv / putenv ---- */
    {
        if (setenv("alloc_e", "v1", 1) != 0) return fail("setenv");
        char *g = getenv("alloc_e");
        if (!g || g[0] != 'v' || g[1] != '1' || g[2] != '\0') return fail("getenv set");
        /* overwrite=0 keeps the existing value but still returns success. */
        if (setenv("alloc_e", "v2", 0) != 0) return fail("setenv no-overwrite ret");
        g = getenv("alloc_e");
        if (!g || g[1] != '1') return fail("setenv no-overwrite kept old");
        if (setenv("alloc_e", "v2", 1) != 0) return fail("setenv overwrite");
        g = getenv("alloc_e");
        if (!g || g[1] != '2') return fail("setenv overwrite value");
        unsetenv("alloc_e");
        if (getenv("alloc_e") != 0) return fail("unsetenv");
        if (putenv("alloc_p=zz") != 0) return fail("putenv");
        g = getenv("alloc_p");
        if (!g || g[0] != 'z' || g[1] != 'z' || g[2] != '\0') return fail("getenv putenv");
    }
    srl("[alloctest] setenv/getenv/unsetenv/putenv ok\n");

    /* ---- 14. strtok_r (consecutive delimiters skipped) + strerror ---- */
    {
        char s[] = "a,bb,,c";
        char *sv = 0;
        char *t1 = strtok_r(s, ",", &sv);
        char *t2 = strtok_r(0, ",", &sv);
        char *t3 = strtok_r(0, ",", &sv);
        char *t4 = strtok_r(0, ",", &sv);
        if (!t1 || t1[0] != 'a' || t1[1] != '\0') return fail("strtok_r t1");
        if (!t2 || t2[0] != 'b' || t2[1] != 'b' || t2[2] != '\0') return fail("strtok_r t2");
        if (!t3 || t3[0] != 'c' || t3[1] != '\0') return fail("strtok_r t3");
        if (t4 != 0) return fail("strtok_r end");

        if (strerror(0)[0] != 'S') return fail("strerror(0)");        /* "Success" */
        if (strerror(ENOENT)[0] != 'N') return fail("strerror ENOENT"); /* "No such ..." */
    }
    srl("[alloctest] strtok_r/strerror ok\n");

    /* ---- 15. bsearch over a sorted array ---- */
    {
        int arr2[] = { 1, 3, 5, 7, 9, 11 };
        int key = 7;
        int *r = (int *)bsearch(&key, arr2, 6, sizeof(int), cmp_int);
        if (!r || *r != 7) return fail("bsearch hit");
        int key2 = 8;
        if (bsearch(&key2, arr2, 6, sizeof(int), cmp_int) != 0) return fail("bsearch miss");
    }
    srl("[alloctest] bsearch ok\n");

    /* ---- 16. pipe + dup roundtrip (single process) ---- */
    {
        int pp[2];
        if (pipe(pp) != 0) return fail("pipe()");
        if (write(pp[1], "Zq", 2) != 2) return fail("pipe write");
        int nd = dup(pp[1]);
        if (nd < 3) return fail("dup() fd");
        if (write(nd, "Yx", 2) != 2) return fail("dup write");
        char rb[5];
        if (read(pp[0], rb, 4) != 4) return fail("pipe read");
        if (rb[0] != 'Z' || rb[1] != 'q' || rb[2] != 'Y' || rb[3] != 'x')
            return fail("pipe/dup content");
        close(pp[0]); close(pp[1]); close(nd);
    }
    srl("[alloctest] pipe/dup ok\n");

    /* ---- 17. time: gmtime_r / mktime roundtrip / strftime ---- *
     * 1000000000 == 2001-09-09 01:46:40 UTC. */
    {
        struct tm tmv;
        time_t t = 1000000000;
        gmtime_r(&t, &tmv);
        if (tmv.tm_year != 101 || tmv.tm_mon != 8 || tmv.tm_mday != 9)
            return fail("gmtime ymd");
        if (tmv.tm_hour != 1 || tmv.tm_min != 46 || tmv.tm_sec != 40)
            return fail("gmtime hms");
        if (mktime(&tmv) != t) return fail("mktime roundtrip");
        char buf[32];
        unsigned int n = strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
        if (n != 19) return fail("strftime len");
        if (buf[0] != '2' || buf[4] != '-' || buf[10] != ' ' || buf[13] != ':')
            return fail("strftime fmt");
    }
    srl("[alloctest] gmtime/mktime/strftime ok\n");

    /* ---- 18. system(): shell available + exit-status round-trip ---- */
    {
        if (system((const char *)0) == 0) return fail("system(NULL) should be nonzero");
        if (system("true")  != 0) return fail("system(\"true\") != 0");
        if (system("false") == 0) return fail("system(\"false\") == 0");
    }
    srl("[alloctest] system ok\n");

    srl("[alloctest] PASS\n");
    return 0;
}
