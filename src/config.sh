SYSTEM_HEADER_PROJECTS="src/libc src/kernel"
PROJECTS="src/libc src/kernel src/userspace"

export MAKE=${MAKE:-make}
export HOST=${HOST:-$(./src/default-host.sh)}

export AR=${HOST}-ar
export AS=${HOST}-as
export CC=${HOST}-gcc

# Transparently route the cross-compiler through ccache when it's available.
# CCACHE=0 disables the wrap (e.g. for one-off measurements of cold builds).
# The CCACHE_DIR / CCACHE_MAXSIZE env vars are inherited from the build
# environment (Dockerfile sets them to /work/.ccache and 500M).
if [ "${CCACHE:-1}" != "0" ] && command -v ccache >/dev/null 2>&1; then
  export CC="ccache $CC"
fi

export PREFIX=/usr
export EXEC_PREFIX=$PREFIX
export BOOTDIR=/boot
export LIBDIR=$EXEC_PREFIX/lib
export INCLUDEDIR=$PREFIX/include

export CFLAGS="${CFLAGS:--O2 -g}"
export CPPFLAGS="${CPPFLAGS:-}"

# Configure the cross-compiler to use the desired system root.
export SYSROOT="$(pwd)/sysroot"
export CC="$CC --sysroot=$SYSROOT"

# Work around that the -elf gcc targets doesn't have a system include directory
# because it was configured with --without-headers rather than --with-sysroot.
if echo "$HOST" | grep -Eq -- '-elf($|-)'; then
  export CC="$CC -isystem=$INCLUDEDIR"
fi
