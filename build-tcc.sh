#!/bin/sh
# build-tcc.sh -- Phase-3 follow-up: cross-build TinyCC for Makar.
#
# STATUS: bring-up scaffolding only.  The userspace sysroot (libc.a,
# crt0.o, /usr/include) and the kernel-side /usr resolver + /tmp
# ramdisk are wired up by the rest of this slice and ship in the
# default `./run.sh iso build`; this script is the next-slice
# launchpad for actually compiling vendor/tinycc against the shim.
#
# What this script *does* today:
#   - Confirms vendor/tinycc/ is populated.
#   - Probes the compile against the Makar sysroot under
#     vendor/tinycc/build-tcc.log so the operator can read the
#     concrete porting gaps surfaced by i686-elf-gcc.
#
# What this script does NOT do (deferred to the TCC-port follow-up):
#   - Produce src/userspace/tcc.elf.  Upstream TCC 0.9.27 pulls
#     <signal.h>, <sys/ucontext.h>, <sys/mman.h>, <struct tm>, the
#     fseek/ftell family, etc.  Each gap needs either a stub backed
#     by a real Makar syscall, or a `#ifdef MAKAR` cordon around the
#     dependent TCC code (mostly tccrun.c -- the in-memory JIT --
#     which Makar can't host without mmap PROT_EXEC anyway).
#
# Once `vendor/tinycc/tcc.o` builds clean, the final link step is:
#     i686-elf-ld -T src/userspace/link.ld -nostdlib -static \
#         src/userspace/crt0.o vendor/tinycc/tcc.o src/userspace/libc.a \
#         -o src/userspace/tcc.elf
# (already present at the foot of this script for when the porting
# work is in.)
#
# Usage:  ./build-tcc.sh
#
# Environment:
#   DOCKER_IMAGE     Docker image to compile inside (default upstream toolchain).
#   DOCKER_BIN       Docker binary (default `docker`).

set -e

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TCC_DIR="$REPO_ROOT/vendor/tinycc"
USER_DIR="$REPO_ROOT/src/userspace"
DOCKER_BIN=${DOCKER_BIN:-docker}
DOCKER_IMAGE=${DOCKER_IMAGE:-arawn780/gcc-cross-i686-elf:fast}
DOCKER_PLATFORM=${DOCKER_PLATFORM:-linux/amd64}

if [ ! -d "$TCC_DIR" ] || [ ! -f "$TCC_DIR/tcc.c" ]; then
    echo "==> build-tcc: vendor/tinycc/ not populated; nothing to do."
    echo "    See vendor/tinycc/README.md for the extraction recipe."
    exit 0
fi

if [ ! -f "$USER_DIR/crt0.o" ] || [ ! -f "$USER_DIR/libc.a" ]; then
    echo "ERROR: src/userspace/{crt0.o,libc.a} missing -- run './run.sh iso build' first." >&2
    exit 1
fi

# Choose build context.
if [ -f /.dockerenv ] && command -v i686-elf-gcc >/dev/null 2>&1; then
    RUN() { bash -lc "$1"; }
elif command -v i686-elf-gcc >/dev/null 2>&1; then
    RUN() { bash -lc "$1"; }
elif command -v "$DOCKER_BIN" >/dev/null 2>&1; then
    RUN() {
        "$DOCKER_BIN" run --rm --platform "$DOCKER_PLATFORM" \
            -u "$(id -u):$(id -g)" \
            -v "$REPO_ROOT:/work" -w /work \
            "$DOCKER_IMAGE" bash -lc "$1"
    }
else
    echo "ERROR: build-tcc needs i686-elf-gcc on PATH or Docker installed." >&2
    exit 1
fi

# Hand-rolled config.h.  Upstream's ./configure assumes a hosted Linux
# build environment we don't have; the CONFIG_* paths it normally emits
# are all supplied as compiler -D flags below.
cat > "$TCC_DIR/config.h" << 'EOF'
/* Hand-rolled config.h for the Makar cross-build.  The compiler -D
 * flags supplied by build-tcc.sh carry all real configuration; this
 * file just satisfies the `#include "config.h"` line in tcc.h. */
EOF

# Apply any porting patches.
RUN "cd vendor/tinycc && for p in patches/[0-9]*.patch; do [ -e \$p ] || continue; if ! patch -p1 --dry-run -R < \$p >/dev/null 2>&1; then patch -p1 < \$p; fi; done"

# Stub headers TCC pulls in but our shim doesn't ship yet.  These let
# the build progress past `#include` resolution; many TCC functions
# referencing them still won't link.  Iterating these into real
# backends is the bulk of the TCC port.
STUB_INC="$TCC_DIR/build-stubs"
mkdir -p "$STUB_INC/sys"

cat > "$STUB_INC/errno.h" << 'EOF'
#ifndef _MAKAR_TCC_ERRNO_H
#define _MAKAR_TCC_ERRNO_H
extern int errno;
#define EPERM   1
#define ENOENT  2
#define EIO     5
#define ENOMEM 12
#define EEXIST 17
#define EINVAL 22
#endif
EOF

cat > "$STUB_INC/fcntl.h" << 'EOF'
#ifndef _MAKAR_TCC_FCNTL_H
#define _MAKAR_TCC_FCNTL_H
#include <syscall.h>   /* O_* live here in the shim */
int open(const char *p, int f, ...);
#endif
EOF

cat > "$STUB_INC/time.h" << 'EOF'
#ifndef _MAKAR_TCC_TIME_H
#define _MAKAR_TCC_TIME_H
typedef unsigned int time_t;
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};
time_t        time(time_t *t);
struct tm    *localtime(const time_t *t);
#endif
EOF

cat > "$STUB_INC/math.h" << 'EOF'
#ifndef _MAKAR_TCC_MATH_H
#define _MAKAR_TCC_MATH_H
double ldexp(double x, int e);
double frexp(double x, int *e);
#endif
EOF

cat > "$STUB_INC/assert.h" << 'EOF'
#ifndef _MAKAR_TCC_ASSERT_H
#define _MAKAR_TCC_ASSERT_H
#define assert(x) ((void)0)
#endif
EOF

cat > "$STUB_INC/sys/stat.h" << 'EOF'
#ifndef _MAKAR_TCC_SYS_STAT_H
#define _MAKAR_TCC_SYS_STAT_H
#include <syscall.h>
int stat(const char *p, struct stat *s);
int fstat(int fd, struct stat *s);
int chmod(const char *p, unsigned int m);
#endif
EOF

cat > "$STUB_INC/sys/time.h" << 'EOF'
#ifndef _MAKAR_TCC_SYS_TIME_H
#define _MAKAR_TCC_SYS_TIME_H
struct timeval { unsigned int tv_sec; unsigned int tv_usec; };
int gettimeofday(struct timeval *tv, void *tz);
#endif
EOF

cat > "$STUB_INC/sys/mman.h" << 'EOF'
#ifndef _MAKAR_TCC_SYS_MMAN_H
#define _MAKAR_TCC_SYS_MMAN_H
/* Used by tccrun.c (TCC's JIT).  Makar can't host PROT_EXEC mmap until
 * SYS_MMAP lands, so the JIT path stays inert. */
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4
#define MAP_PRIVATE  2
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED   ((void *)-1)
void *mmap(void *a, unsigned int sz, int prot, int fl, int fd, long off);
int   munmap(void *a, unsigned int sz);
#endif
EOF

cat > "$STUB_INC/sys/ucontext.h" << 'EOF'
#ifndef _MAKAR_TCC_SYS_UCONTEXT_H
#define _MAKAR_TCC_SYS_UCONTEXT_H
typedef struct { int dummy; } ucontext_t;
#endif
EOF

cat > "$STUB_INC/inttypes.h" << 'EOF'
#ifndef _MAKAR_TCC_INTTYPES_H
#define _MAKAR_TCC_INTTYPES_H
#include <stdint.h>
#endif
EOF

cat > "$STUB_INC/stdint.h" << 'EOF'
#ifndef _MAKAR_TCC_STDINT_H
#define _MAKAR_TCC_STDINT_H
#include <stddef.h>
typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef int                int32_t;
typedef unsigned int       uint32_t;
typedef long long          int64_t;
typedef unsigned long long uint64_t;
typedef unsigned int       uintptr_t;
typedef int                intptr_t;
typedef int                ssize_t;
#endif
EOF

cat > "$STUB_INC/stdbool.h" << 'EOF'
#ifndef _MAKAR_TCC_STDBOOL_H
#define _MAKAR_TCC_STDBOOL_H
#define bool  _Bool
#define true  1
#define false 0
#endif
EOF

cat > "$STUB_INC/signal.h" << 'EOF'
#ifndef _MAKAR_TCC_SIGNAL_H
#define _MAKAR_TCC_SIGNAL_H
typedef void (*sighandler_t)(int);
#endif
EOF

cat > "$STUB_INC/unistd.h" << 'EOF'
#ifndef _MAKAR_TCC_UNISTD_H
#define _MAKAR_TCC_UNISTD_H
typedef int ssize_t;
int    close(int fd);
int    unlink(const char *p);
long   read(int fd, void *b, unsigned n);
long   write(int fd, const void *b, unsigned n);
long   lseek(int fd, long o, int w);
#endif
EOF

COMMON_DEFS="\
 -DTCC_TARGET_I386 \
 -DTCC_VERSION=\\\"0.9.27-makar\\\" \
 -DCONFIG_TCCDIR=\\\"/usr/lib/tcc\\\" \
 -DCONFIG_TCC_SYSINCLUDEPATHS=\\\"/usr/include\\\" \
 -DCONFIG_TCC_LIBPATHS=\\\"/usr/lib\\\" \
 -DCONFIG_TCC_CRTPREFIX=\\\"/usr/lib\\\" \
 -DCONFIG_LDDIR=\\\"lib\\\" \
 -DCONFIG_TCC_STATIC \
 -DCONFIG_TCCBOOT \
 -DCONFIG_TRIPLET=\\\"i386-makar\\\" \
 -DONE_SOURCE=1"

echo "==> Probing tcc.c against the Makar shim (porting smoke test) ..."
RUN "cd vendor/tinycc && \
    i686-elf-gcc -O0 -g -std=gnu99 -ffreestanding -fno-stack-protector \
        -Wall -Wno-unused-parameter -Wno-pointer-sign -Wno-pointer-to-int-cast \
        -Wno-int-to-pointer-cast -Wno-format -Wno-missing-field-initializers \
        -nostdlib \
        -I. -I build-stubs \
        -I ../../src/userspace \
        $COMMON_DEFS \
        -c tcc.c -o tcc.o 2>&1 | tee build-tcc.log | tail -40" || true

if ! RUN "test -f vendor/tinycc/tcc.o"; then
    cat <<'EOF'

==> Smoke-test status: tcc.c does NOT yet compile against the Makar
    libc shim.  The log at vendor/tinycc/build-tcc.log enumerates the
    remaining porting gaps.  Headline residual deltas as of v0.9.27:

      - tccrun.c (TCC's JIT) drags <signal.h>, <sys/ucontext.h>,
        <sys/mman.h>, SA_RESETHAND, siginfo_t -- all needed only by
        the `-run` path Makar can't host.  Cordon with a fresh
        CONFIG_TCC_NO_RUN ifdef under vendor/tinycc/patches/.
      - tccpp.c references `struct tm` from <time.h>; either stub a
        real /proc/rtc-backed localtime() or guard the __DATE__/
        __TIME__ feature.
      - tccelf.c uses `ssize_t` widely (now declared in our stdint.h
        stub, but the surrounding declarations need a wider review).
      - libtcc.c calls fdopen/fseek/ftell/exit/strtoll -- all
        additions to the userspace libc shim (or new TCC patches that
        route to the syscall layer directly).

    None of these are blockers for the rest of the slice; the
    sysroot + /tmp + /usr scaffolding is committed in this PR and
    `./run.sh iso build` produces a working OS image without tcc.elf.

EOF
    exit 0
fi

echo "==> Linking tcc.elf at USER_CODE_BASE = 0x40000000 ..."
# Pull libgcc.a alongside libc.a to resolve gcc-emitted helpers
# (__udivdi3, __umoddi3, ...) used by TCC's 64-bit integer paths.
RUN "i686-elf-ld -T src/userspace/link.ld -nostdlib -static \
    src/userspace/crt0.o vendor/tinycc/tcc.o src/userspace/libc.a \
    \$(i686-elf-gcc -print-libgcc-file-name) \
    -o src/userspace/tcc.elf"

echo "==> tcc.elf built:"
RUN "ls -l src/userspace/tcc.elf"
