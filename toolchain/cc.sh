#!/bin/sh
# cc.sh -- invoke the hosted cross compiler, Docker-wrapped for cross-platform
# use.  The toolchain in install/ is built as Linux x86-64 binaries, so on
# macOS/Windows it must run inside the Linux container; this wrapper handles
# that transparently (same execution strategy as run.sh / build-musl-cross.sh).
#
# Usage:
#   toolchain/cc.sh -static -march=i686 hello.c -o hello
# The current directory is mounted at /src inside the container, so relative
# paths in your gcc args work as expected.
set -e

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
IMAGE="${MAKAR_TOOLCHAIN_IMAGE:-makar-toolchain:local}"
DOCKER_BIN="${DOCKER_BIN:-docker}"
GCC_REL="install/bin/i686-linux-musl-gcc"

if [ ! -x "$DIR/$GCC_REL" ]; then
    echo "ERROR: toolchain not built -- run ./toolchain/build-musl-cross.sh first." >&2
    exit 1
fi

# Already inside a container, or no Docker -> run the binary directly.
if [ -f /.dockerenv ] || ! command -v "$DOCKER_BIN" >/dev/null 2>&1; then
    exec "$DIR/$GCC_REL" "$@"
fi

# Host with Docker -> run the compiler inside the toolchain container, with
# the toolchain at /work and the caller's cwd at /src.
exec "$DOCKER_BIN" run --rm \
    -v "$DIR:/work" -v "$PWD:/src" -w /src \
    "$IMAGE" "/work/$GCC_REL" "$@"
