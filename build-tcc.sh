#!/bin/sh
# build-tcc.sh -- Cross-build TinyCC (v0.9.27) as a Makar userspace app.
#
# Produces:
#   src/userspace/tcc.elf       The compiler binary (ring 3, 0x40000000)
#
# Also stages TCC's sysroot artifacts into isodir/ (ready for grub-mkrescue):
#   isodir/usr/lib/tcc/libtcc1.a       TCC's runtime library
#   isodir/usr/lib/tcc/include/        TCC's built-in headers (stdarg, stddef, etc.)
#   isodir/usr/lib/crt1.o              CRT startup (copy of crt0.o)
#   isodir/usr/lib/crti.o              Empty init stub
#   isodir/usr/lib/crtn.o              Empty fini stub
#
# Called from iso.sh after build.sh has produced crt0.o and libc.a.
# Also works standalone: ./build-tcc.sh
#
# Environment:
#   DOCKER_IMAGE     Docker image to compile inside (default upstream toolchain).
#   DOCKER_BIN       Docker binary (default `docker`).

set -e

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TCC_DIR="$REPO_ROOT/vendor/tinycc"
USER_DIR="$REPO_ROOT/src/userspace"
ISO_DIR="$REPO_ROOT/isodir"
DOCKER_BIN=${DOCKER_BIN:-docker}
DOCKER_IMAGE=${DOCKER_IMAGE:-arawn780/gcc-cross-i686-elf:fast}
DOCKER_PLATFORM=${DOCKER_PLATFORM:-linux/amd64}

if [ ! -d "$TCC_DIR" ] || [ ! -f "$TCC_DIR/tcc.c" ]; then
    echo "==> build-tcc: vendor/tinycc/ not populated; skipping."
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

# ── config.h ─────────────────────────────────────────────────────────────────
# Upstream's ./configure assumes a hosted Linux environment; we supply all
# CONFIG_* values via -D flags instead.
cat > "$TCC_DIR/config.h" << 'EOF'
/* Hand-rolled config.h for the Makar cross-build.  The compiler -D
 * flags supplied by build-tcc.sh carry all real configuration; this
 * file just satisfies the `#include "config.h"` line in tcc.h. */
EOF

# ── Apply porting patches ────────────────────────────────────────────────────
RUN "cd vendor/tinycc && for p in patches/[0-9]*.patch; do [ -e \$p ] || continue; if ! patch -p1 --dry-run -R < \$p >/dev/null 2>&1; then patch -p1 < \$p; fi; done"

# ── Stub headers ─────────────────────────────────────────────────────────────
# These satisfy TCC source-level #includes that reference POSIX/hosted headers
# which Makar doesn't ship.  The real implementations live in tcc_compat.c.
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

# ── Common -D flags ──────────────────────────────────────────────────────────
COMMON_DEFS="\
 -DTCC_TARGET_I386 \
 -DTCC_VERSION=\\\"0.9.27-makar\\\" \
 -DCONFIG_TCCDIR=\\\"/usr/lib/tcc\\\" \
 -DCONFIG_TCC_SYSINCLUDEPATHS=\\\"/usr/include:{B}/include\\\" \
 -DCONFIG_TCC_LIBPATHS=\\\"/usr/lib\\\" \
 -DCONFIG_TCC_CRTPREFIX=\\\"/usr/lib\\\" \
 -DCONFIG_LDDIR=\\\"lib\\\" \
 -DCONFIG_TCC_STATIC \
 -DCONFIG_TCCBOOT \
 -DONE_SOURCE=1"

# ── Compile tcc.o ────────────────────────────────────────────────────────────
echo "==> Compiling tcc.c ..."
RUN "cd vendor/tinycc && \
    i686-elf-gcc -O2 -g -std=gnu99 -ffreestanding -fno-stack-protector \
        -Wall -Wno-unused-parameter -Wno-pointer-sign -Wno-pointer-to-int-cast \
        -Wno-int-to-pointer-cast -Wno-format -Wno-missing-field-initializers \
        -Wno-unused-function \
        -nostdlib \
        -I. -I build-stubs \
        -I ../../src/userspace \
        $COMMON_DEFS \
        -c tcc.c -o tcc.o 2>&1 | tee build-tcc.log | tail -20" || true

if ! RUN "test -f vendor/tinycc/tcc.o"; then
    echo "ERROR: tcc.c failed to compile.  See vendor/tinycc/build-tcc.log." >&2
    exit 1
fi

# ── Link tcc.elf ─────────────────────────────────────────────────────────────
echo "==> Linking tcc.elf at USER_CODE_BASE = 0x40000000 ..."
RUN "i686-elf-ld -T src/userspace/link.ld -nostdlib -static \
    src/userspace/crt0.o vendor/tinycc/tcc.o src/userspace/libc.a \
    \$(i686-elf-gcc -print-libgcc-file-name) \
    -o src/userspace/tcc.elf"
echo "==> tcc.elf: $(ls -lh src/userspace/tcc.elf 2>/dev/null | awk '{print $5}')"

# ── Build libtcc1.a (TCC's runtime library for compiled programs) ────────────
echo "==> Building libtcc1.a ..."
RUN "cd vendor/tinycc/lib && \
    i686-elf-gcc -O2 -ffreestanding -fno-stack-protector -nostdlib \
        -c libtcc1.c -o libtcc1.o && \
    i686-elf-gcc -O2 -ffreestanding -fno-stack-protector -nostdlib \
        -c alloca86.S -o alloca86.o && \
    i686-elf-ar rcs libtcc1.a libtcc1.o alloca86.o"
echo "==> libtcc1.a: $(ls -lh vendor/tinycc/lib/libtcc1.a 2>/dev/null | awk '{print $5}')"

# ── Build CRT stubs for TCC's default link sequence ─────────────────────────
# TCC's default link is: crt1.o + crti.o + <user objects> + -lc + crtn.o
# crt1.o = our crt0.o (provides _start)
# crti.o / crtn.o = empty stubs (no init/fini section hooks)
echo "==> Building CRT stubs ..."
RUN "cp src/userspace/crt0.o src/userspace/crt1.o && \
    echo '.section .init' | i686-elf-as --32 -o src/userspace/crti.o && \
    echo '.section .fini' | i686-elf-as --32 -o src/userspace/crtn.o"

# ── Stage TCC sysroot into isodir ───────────────────────────────────────────
echo "==> Staging TCC sysroot into isodir/ ..."
mkdir -p "$ISO_DIR/usr/lib/tcc/include"

# TCC's own built-in headers (stdarg.h, stddef.h, stdbool.h, float.h, varargs.h)
cp "$TCC_DIR/include/"*.h "$ISO_DIR/usr/lib/tcc/include/"

# libtcc1.a goes under the TCC lib path
cp "$TCC_DIR/lib/libtcc1.a" "$ISO_DIR/usr/lib/tcc/"

# CRT objects alongside libc.a at /usr/lib
cp "$USER_DIR/crt1.o" "$ISO_DIR/usr/lib/"
cp "$USER_DIR/crti.o" "$ISO_DIR/usr/lib/"
cp "$USER_DIR/crtn.o" "$ISO_DIR/usr/lib/"

echo "==> TCC build complete."
echo "    tcc.elf will be shipped at /mnt/cdrom/apps/tcc.elf"
echo "    Sysroot: /usr/lib/tcc/ (libtcc1.a + include/)"
echo "    CRT: /usr/lib/{crt1,crti,crtn}.o"
