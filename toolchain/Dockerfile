# Build environment for the Makar hosted cross toolchain (musl-cross-make).
# Just the deps -- the actual binutils/gcc/musl build runs at `docker run`
# time into a mounted volume, so the heavy artefacts never bloat the image.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        bison flex texinfo m4 \
        wget git ca-certificates \
        xz-utils file rsync python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
