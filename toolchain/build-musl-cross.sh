#!/bin/sh
# build-musl-cross.sh -- build the Makar hosted cross toolchain (Docker-first).
#
# Produces i686-linux-musl-{gcc,ld,as,ar,...} + a musl sysroot under
# toolchain/install/.  ~30-60 min on first run; musl-cross-make downloads +
# builds binutils, gcc, musl, gmp, mpfr, mpc itself.
#
# Cross-platform by design (matches run.sh's execution strategy):
#   1. /.dockerenv present (already in a container) -> build directly
#   2. Docker CLI available                         -> build image + run inside
#   3. otherwise                                    -> host tools fallback
#
# Output binaries are static musl i386 ELFs (Linux i386 int 0x80 ABI).  Making
# Makar *run* them is separate kernel work -- see toolchain/README.md.
set -e

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
IMAGE="${MAKAR_TOOLCHAIN_IMAGE:-makar-toolchain:local}"
DOCKER_BIN="${DOCKER_BIN:-docker}"
MCM_URL="https://github.com/richfelker/musl-cross-make"

# The actual build steps.  WORK is the toolchain dir (host path on the host
# fallback, /work inside the container -- both map to the same files).
do_build() {
    WORK="$1"
    SRC="$WORK/musl-cross-make"
    OUT="$WORK/install"
    JOBS=$(nproc 2>/dev/null || echo 4)

    echo "==> Hosted toolchain build (i686-linux-musl) -> $OUT"
    for t in gcc g++ make wget git; do
        command -v "$t" >/dev/null 2>&1 || { echo "ERROR: missing build tool '$t'"; exit 1; }
    done

    if [ ! -d "$SRC/.git" ]; then
        echo "==> Cloning musl-cross-make ..."
        git clone --depth 1 "$MCM_URL" "$SRC"
    fi

    cat > "$SRC/config.mak" <<EOF
TARGET = i686-linux-musl
OUTPUT = $OUT
# C only for the first toolchain; trims build time.
GCC_CONFIG += --enable-languages=c --disable-libquadmath --disable-decimal-float
EOF

    echo "==> Building (binutils + gcc + musl) with -j$JOBS ..."
    ( cd "$SRC" && make -j"$JOBS" && make install )

    echo ""
    echo "==> Toolchain installed: $OUT/bin/i686-linux-musl-gcc"
    "$OUT/bin/i686-linux-musl-gcc" --version 2>/dev/null | head -1 || true
}

# 1. Already inside a container -> run directly (DIR is the mounted /work).
if [ -f /.dockerenv ]; then
    do_build "$DIR"
    exit 0
fi

# 2. Docker available -> build the env image, run the build inside it.
if command -v "$DOCKER_BIN" >/dev/null 2>&1; then
    echo "==> Building toolchain image ($IMAGE) ..."
    "$DOCKER_BIN" build -t "$IMAGE" "$DIR"
    echo "==> Running musl-cross-make inside the container ..."
    "$DOCKER_BIN" run --rm -v "$DIR:/work" -w /work "$IMAGE" \
        sh /work/build-musl-cross.sh
    exit $?
fi

# 3. No Docker -> host tools fallback.
echo "==> Docker not found; building with host tools."
do_build "$DIR"
