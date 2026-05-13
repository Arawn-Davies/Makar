# Dockerfile – Makar OS build environment
#
# Layers ccache on top of the pre-built i686-elf cross-compiler image used by
# CI so that local builds and CI runs share the same toolchain version while
# benefiting from object caching.
#
# The image ships:
#   • i686-elf-gcc / binutils cross-toolchain (from upstream image)
#   • grub-mkrescue, grub-mkimage, grub-file, xorriso
#   • qemu-system-i386, gdb-multiarch, make
#   • ccache (added by this Dockerfile)
#
# Tag the local build as makar-build:local — run.sh and the helper scripts
# pick it up automatically when present, falling back to the upstream image:
#
#   docker build -t makar-build:local .
#
# CCACHE_DIR is set to /work/.ccache so the cache lives inside the bind-
# mounted source tree and persists across container invocations.  .ccache/
# is gitignored.

FROM arawn780/gcc-cross-i686-elf:fast

RUN apt-get update \
 && apt-get install -y --no-install-recommends ccache \
 && rm -rf /var/lib/apt/lists/*

ENV CCACHE_DIR=/work/.ccache \
    CCACHE_COMPRESS=1 \
    CCACHE_MAXSIZE=500M

WORKDIR /work

# Default command: build the bootable ISO.
CMD ["bash", "iso.sh"]
