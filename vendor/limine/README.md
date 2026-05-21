# Vendored limine BIOS stage

`limine-bios.sys` is a prebuilt binary from the **Limine** bootloader project,
taken verbatim from the official `limine-binary` release archive.

- Version: see [`VERSION`](VERSION) (currently v12.3.0)
- Source: https://github.com/limine-bootloader/limine (release `v12.3.0`,
  asset `limine-binary.zip`)
- Licence: BSD-2-Clause (see the Limine repository's `COPYING`)

## Why it's here

Makar's in-kernel `install` command deploys limine to the target HDD from
inside the running OS (in QEMU or on real hardware). It reproduces what
`limine bios-install` does for an MBR disk, using only this single file:

1. `limine-bios.sys[0:512)` is written to the target's MBR (sector 0),
   **preserving** the existing partition table (bytes 446-509) and disk
   timestamp (bytes 218-223), exactly as the host tool does.
2. `limine-bios.sys[512:]` (stage 2) is written into the post-MBR embedding
   gap starting at sector 1.
3. The boot sector's stage-2 location field at offset `0x1a4` is patched with
   the u64 byte offset `512` (little-endian).
4. The whole `limine-bios.sys` file plus a generated `limine.conf` are copied
   to `/limine/` on the freshly formatted rootfs.

The MBR install algorithm mirrors `host/limine.c::bios_install()` at the
matching upstream tag; keep this binary and that reference in sync when
bumping the version.

## Updating

```sh
gh release download v<X.Y.Z> --repo limine-bootloader/limine \
    --pattern 'limine-binary.zip'
unzip -j limine-binary.zip 'limine-binary/limine-bios.sys' -d vendor/limine
echo 'v<X.Y.Z>' > vendor/limine/VERSION
```

Then re-check `host/limine.c::bios_install()` at that tag for any change to the
`0x1a4` stage-2 location encoding (it changed between v8.x and v12.x).

The host build (`iso.sh`, `generate-hdd.sh`) is unaffected — it still boots via
GRUB. Only the runtime installer consumes this file.
