#!/bin/sh
set -e

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DOCKER_BIN=${DOCKER_BIN:-docker}
DOCKER_IMAGE=${DOCKER_IMAGE:-arawn780/gcc-cross-i686-elf:fast}

if ! command -v "$DOCKER_BIN" >/dev/null 2>&1; then
    echo "ERROR: Docker CLI not found (expected '$DOCKER_BIN')." >&2
    exit 1
fi

echo "==> Building makar.iso in Docker image: $DOCKER_IMAGE"
# Forward CFLAGS/CPPFLAGS from the caller's environment (e.g. CFLAGS='-O0 -g3'
# CPPFLAGS='-DTEST_MODE') by embedding them in the bash command string.
_build_cmd="bash iso.sh"
if [ -n "$CFLAGS" ] && [ -n "$CPPFLAGS" ]; then
    _build_cmd="CFLAGS='$CFLAGS' CPPFLAGS='$CPPFLAGS' bash iso.sh"
elif [ -n "$CFLAGS" ]; then
    _build_cmd="CFLAGS='$CFLAGS' bash iso.sh"
elif [ -n "$CPPFLAGS" ]; then
    _build_cmd="CPPFLAGS='$CPPFLAGS' bash iso.sh"
fi
"$DOCKER_BIN" run --rm \
    -u "$(id -u):$(id -g)" \
    -v "$REPO_ROOT:/work" \
    -w /work \
    "$DOCKER_IMAGE" \
    bash -lc "$_build_cmd"

echo "==> Docker build complete: $REPO_ROOT/makar.iso"
