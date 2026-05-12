# HDD boot - installed image bootable without CD-ROM

> **Status:** complete
> **Branch:** `fix/hdd-boot` (awaiting PR merge)

## Summary

Installed HDD boot path: raw MBR + FAT32 + GRUB 2 image (`makar-hdd.img`)
bootable with `-boot c`, fully self-contained (no CD-ROM dependency).
Separate `makar-hdd-test.img` for CI testing.

## Implemented

- [x] `generate-hdd.sh` - creates 512 MiB MBR + FAT32 HDD image using
      `grub-mkimage` (explicit `(hd0,msdos1)` prefix, no UUID probe)
- [x] `generate-hdd.sh --test` - same image but grub.cfg passes `test`
      kernel arg (foundation for runtime test-mode switching)
- [x] `docker-hdd-boot.sh` - clean-build + interactive QEMU HDD boot
- [x] `docker-hdd-test.sh` - clean-build + GDB boot test with separate
      `makar-hdd-test.img` (never reuses interactive image)
- [x] IDE SRST fix: software reset before probing so GRUB's transient
      channel state doesn't cause drive 0 to be silently skipped
- [x] Apps directory (`isodir/apps/`) copied to HDD at `/apps/`
- [x] `Dockerfile.compiler` - documents `arawn780/gcc-cross-i686-elf:fast`;
      now includes `dosfstools` + `fdisk` so one image covers everything
- [x] GDB HDD test: `hdd_mount` group waits for `keyboard_getchar` before
      checking `fat32_mounted()` to avoid a race with `timer_callback`
- [x] Parallel builds: `build.sh` uses `-j$(nproc)` automatically

## TODO

- [ ] Runtime test-mode via GRUB kernel arg (`multiboot2 /boot/makar.kernel test`)
      - `kernel.c` needs to parse `MULTIBOOT2_TAG_TYPE_CMDLINE` (type 1) and
      set a `test_mode` flag, replacing `#ifdef TEST_MODE` guards with
      `if (test_mode)` checks so ISO and HDD can share one binary
