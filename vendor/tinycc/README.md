# Vendored TinyCC (TCC)

Tarball-extracted snapshot of the TinyCC compiler used by Makar to ship
`tcc.elf` as an in-OS C compiler.

- **Upstream version**: see `UPSTREAM_VERSION` (currently `0.9.27`,
  release tag `release_0_9_27`).
- **Source**:
  <https://github.com/TinyCC/tinycc/tree/release_0_9_27> (GitHub mirror
  of the canonical `https://repo.or.cz/tinycc.git`).
- **Licence**: LGPL-2.1 (see [`COPYING`](COPYING)).

## Why it's here

Makar's roadmap calls for an in-OS C compiler (see `docs/tcc-feasibility.md`).
TCC is portable, has a first-class i386 backend, and fits in well under a
megabyte once stripped — which is exactly what we need for "boot,
write code, compile, run" on bare metal.

The full upstream source ships unmodified inside this directory; any
porting deltas live as a numbered series under `patches/` and are
applied by `build-tcc.sh` before `./configure`.

## Build

The kernel build doesn't depend on TCC.  `tcc.elf` is built by the
sibling `build-tcc.sh` at the repo root (sibling of `build.sh`,
`generate-hdd.sh`).  Running `./run.sh iso build` from a checkout
without TCC built still produces a working OS image — only the
`tcc` shell command is missing.

## Updating

```sh
curl -sSL -o /tmp/tcc.tar.gz \
    "https://codeload.github.com/TinyCC/tinycc/tar.gz/refs/tags/release_X_Y_Z"
rm -rf vendor/tinycc
mkdir -p vendor/tinycc
tar -xzf /tmp/tcc.tar.gz -C /tmp/
mv /tmp/tinycc-release_X_Y_Z/* vendor/tinycc/
mv vendor/tinycc/VERSION vendor/tinycc/UPSTREAM_VERSION
echo 'release_X_Y_Z (upstream TinyCC vX.Y.Z)' > vendor/tinycc/VERSION
# Re-apply patches/ on the new source; investigate breakage.
```

Then rebuild via `./build-tcc.sh` and confirm `tests/ui_test.sh tcc_hello`
still passes.
