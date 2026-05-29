#!/bin/sh
# build-kernel-tcc.sh -- Rebuild the Makar kernel using TCC.
#
# Two roles:
#   1. Host driver -- compile + link the kernel here (fast iteration on the
#      build recipe; output build/ktcc/makar.kernel.tcc).
#   2. Code generator -- emit src/userspace/rebuild-kernel.sh, the in-OS
#      kernel-sh script that does the exact same build from inside Makar.
#
# We MUST generate the in-OS script: kernel sh has no command-substitution,
# no `expr`, no string-manip, and SHELL_MAX_ARGS=16 so we can't pass all
# objects in one tcc link.  We work around that with explicit numbered .o
# names + a static-archive (libk.a) link, all emitted from this script.

set -e

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$REPO_ROOT"

TCC=vendor/tinycc/i386-tcc
LIBTCC1=vendor/tinycc/lib/libtcc1.a
B=build/ktcc

if [ ! -x "$TCC" ]; then
    echo "ERROR: $TCC not found.  Build it first:" >&2
    echo "  (cd vendor/tinycc && ./configure --cpu=i386 --enable-cross && make i386-tcc)" >&2
    exit 1
fi
[ -f "$LIBTCC1" ] || { echo "ERROR: $LIBTCC1 missing" >&2; exit 1; }

CF="-ffreestanding -D__is_kernel -DDEV_BUILD \
    -Ivendor/tinycc/include -Ivendor/tinycc/build-stubs \
    -Isrc/kernel/include -Isrc/libc/include"

rm -rf "$B" && mkdir -p "$B"

# Sentinel kernel_end (linker script would normally provide this).
cat > "$B/kernel_end.S" << 'EOF'
.section .bss
.global _kernel_end
_kernel_end:
.byte 0
EOF

# Canonical file list (single source of truth for both host + in-OS path).
# boot.S is special-cased: must compile + link FIRST so MB2 header lands in
# the first 32 KiB of .text.  Order otherwise mirrors arch/i386/make.config.
KFILES="
src/kernel/arch/i386/boot/boot.S
src/kernel/arch/i386/boot/mb2_header_check.c
src/kernel/arch/i386/boot/crti.S
src/kernel/arch/i386/boot/crtn.S
src/kernel/arch/i386/core/dt_asm.S
src/kernel/arch/i386/core/descr_tbl.c
src/kernel/arch/i386/core/isr_asm.S
src/kernel/arch/i386/core/isr.c
src/kernel/arch/i386/drivers/serial.c
src/kernel/arch/i386/drivers/timer.c
src/kernel/arch/i386/drivers/keyboard.c
src/kernel/arch/i386/drivers/ide.c
src/kernel/arch/i386/drivers/partition.c
src/kernel/arch/i386/drivers/acpi.c
src/kernel/arch/i386/drivers/rtc.c
src/kernel/arch/i386/fs/fat32.c
src/kernel/arch/i386/fs/ext2.c
src/kernel/arch/i386/fs/iso9660.c
src/kernel/arch/i386/fs/procfs.c
src/kernel/arch/i386/fs/devfs.c
src/kernel/arch/i386/fs/logfs.c
src/kernel/arch/i386/fs/tmpfs.c
src/kernel/arch/i386/fs/vfs.c
src/kernel/arch/i386/mm/pmm.c
src/kernel/arch/i386/mm/paging.c
src/kernel/arch/i386/mm/heap.c
src/kernel/arch/i386/mm/vmm.c
src/kernel/arch/i386/display/tty.c
src/kernel/arch/i386/display/vesa.c
src/kernel/arch/i386/display/vesa_tty.c
src/kernel/arch/i386/display/vt.c
src/kernel/arch/i386/display/bochs_vbe.c
src/kernel/arch/i386/proc/system.c
src/kernel/arch/i386/proc/task_asm.S
src/kernel/arch/i386/proc/task.c
src/kernel/arch/i386/proc/syscall.c
src/kernel/arch/i386/proc/signal.c
src/kernel/arch/i386/proc/fd.c
src/kernel/arch/i386/proc/ring3.S
src/kernel/arch/i386/proc/chainload.S
src/kernel/arch/i386/proc/elf.c
src/kernel/arch/i386/proc/usertest.c
src/kernel/arch/i386/proc/ktest.c
src/kernel/arch/i386/proc/installer.c
src/kernel/arch/i386/proc/vtty.c
src/kernel/arch/i386/shell/shell.c
src/kernel/arch/i386/shell/shell_glob.c
src/kernel/arch/i386/shell/shell_help.c
src/kernel/arch/i386/shell/shell_cmd_display.c
src/kernel/arch/i386/shell/shell_cmd_system.c
src/kernel/arch/i386/shell/shell_cmd_disk.c
src/kernel/arch/i386/shell/shell_cmd_fs.c
src/kernel/arch/i386/shell/shell_cmd_apps.c
src/kernel/arch/i386/shell/shell_cmd_man.c
src/kernel/arch/i386/shell/shell_cmd_script.c
src/kernel/arch/i386/shell/sh_script.c
src/kernel/arch/i386/debug/debug.c
src/kernel/kernel/kernel.c
src/libc/string/memcmp.c
src/libc/string/memcpy.c
src/libc/string/memmove.c
src/libc/string/memset.c
src/libc/string/strcat.c
src/libc/string/strchr.c
src/libc/string/strcmp.c
src/libc/string/strcpy.c
src/libc/string/strlen.c
src/libc/string/strncmp.c
src/libc/string/strncpy.c
src/libc/string/strrchr.c
src/libc/string/strstr.c
src/libc/stdio/printf.c
src/libc/stdio/putchar.c
src/libc/stdio/puts.c
src/libc/stdlib/abort.c
"

# ---------- Host build (proves the recipe end-to-end) ----------------
$TCC $CF -c "$B/kernel_end.S" -o "$B/kernel_end.o"

OBJS=""
N=0
for f in $KFILES; do
    [ -f "$f" ] || { echo "ERROR: missing $f" >&2; exit 1; }
    N=$((N + 1))
    nn=$(printf '%03d' "$N")
    out="$B/$nn.o"
    $TCC $CF -c "$f" -o "$out"
    OBJS="$OBJS $out"
done

$TCC -nostdlib -nostdinc -static \
    -Wl,-Ttext=0x100000 -Wl,-section-alignment=0x1000 \
    -o "$B/makar.kernel.tcc" \
    $OBJS "$LIBTCC1" "$B/kernel_end.o"

if command -v grub-file >/dev/null 2>&1; then
    grub-file --is-x86-multiboot2 "$B/makar.kernel.tcc" \
        || { echo "ERROR: not a valid MB2 kernel" >&2; exit 1; }
fi
echo "==> $B/makar.kernel.tcc ($(wc -c < $B/makar.kernel.tcc) bytes)"

# ---------- Emit in-OS script ----------------------------------------
# Kernel sh constraints we work around here:
#   - echo "..." prints the literal quotes (no quote stripping).  Use
#     bareword args for markers grep'd by the test harness.
#   - No `exit N` / `>` / `>>` / command substitution.
#   - No reliable mid-script abort: accumulate FAILED across all compiles
#     instead, and let the final IF decide PASS / FAIL.
OUT=src/userspace/rebuild-kernel.sh
{
echo "# rebuild-kernel.sh -- GENERATED by build-kernel-tcc.sh.  Do not edit."
echo "# Rebuilds the kernel inside Makar via /apps/tcc.elf, then writes"
echo "# /tmp/makar.kernel.tcc.  Copy to /boot/makar.kernel and reboot to test."
echo ""
echo "echo rebuild-kernel: BEGIN"
echo "mkdir /tmp/ktcc"
echo "FAILED=0"
echo ""
echo "# _kernel_end sentinel is shipped at /apps/kend.S (kernel sh has no > redir)."
echo "exec /apps/tcc.elf -c /apps/kend.S -o /tmp/ktcc/kend.o"
echo "if [ \$? -ne 0 ]; then echo rebuild-kernel: FAIL kend; FAILED=1; fi"
echo ""
} > "$OUT"

N=0
for f in $KFILES; do
    N=$((N + 1))
    nn=$(printf '%03d' "$N")
    in_os="/$f"
    cat >> "$OUT" << EOF
echo rebuild-kernel: cc $f
exec /apps/tcc.elf -ffreestanding -D__is_kernel -DDEV_BUILD -I/usr/include/kernel-build -c $in_os -o /tmp/ktcc/$nn.o
if [ \$? -ne 0 ]; then echo rebuild-kernel: FAIL $f; FAILED=1; fi
EOF
done

{
echo ""
echo "echo rebuild-kernel: archive"
echo "rm /tmp/ktcc/libk.a"
} >> "$OUT"
N=0
for f in $KFILES; do
    N=$((N + 1))
    [ "$N" -eq 1 ] && continue
    nn=$(printf '%03d' "$N")
    echo "exec /apps/tcc.elf -ar rcs /tmp/ktcc/libk.a /tmp/ktcc/$nn.o" >> "$OUT"
done

{
echo ""
echo "echo rebuild-kernel: link"
echo "exec /apps/tcc.elf -nostdlib -nostdinc -static -Wl,-Ttext=0x100000 -Wl,-section-alignment=0x1000 -o /tmp/makar.kernel.tcc /tmp/ktcc/001.o -Wl,--whole-archive /tmp/ktcc/libk.a -Wl,--no-whole-archive /usr/lib/tcc/libtcc1.a /tmp/ktcc/kend.o"
echo "if [ \$? -ne 0 ]; then echo rebuild-kernel: FAIL link; FAILED=1; fi"
echo ""
echo "if [ \$FAILED -eq 0 ]; then"
echo "  echo REBUILD-KERNEL: ALL PASS"
echo "  echo rebuild-kernel: cp /tmp/makar.kernel.tcc /boot/makar.kernel and reboot"
echo "else"
echo "  echo REBUILD-KERNEL: FAIL"
echo "fi"
} >> "$OUT"

echo "==> emitted $OUT ($(wc -l < $OUT) lines)"
