DEFAULT_HOST!=../default-host.sh
HOST?=$(DEFAULT_HOST)

CC=$(HOST)-gcc
AS=$(HOST)-as
LD=$(HOST)-ld
AR=$(HOST)-ar
OBJCOPY=$(HOST)-objcopy

CFLAGS?=-O2 -g
CFLAGS:=$(CFLAGS) -std=c11 -ffreestanding -fno-stack-protector \
        -Wall -Wextra -nostdlib -nostdinc -I. -MMD -MP

LDFLAGS=-T link.ld -nostdlib

# ---- doomgeneric (vendored) ----------------------------------------------
# Core Doom + the generic glue; exclude every foreign platform backend and
# use our own doomgeneric_makar.c.  Compiled against the hosted libc + headers.
DOOM_DIR=../../vendor/doomgeneric/doomgeneric
DOOM_PLATFORMS=allegro emscripten linuxvt sdl soso sosox win xlib
DOOM_EXCL=$(addprefix $(DOOM_DIR)/doomgeneric_,$(addsuffix .c,$(DOOM_PLATFORMS)))
# Sound/music backends: FEATURE_SOUND is off, so drop them (no stub needed).
DOOM_EXCL_SND=$(addprefix $(DOOM_DIR)/,i_allegrosound.c i_allegromusic.c i_sdlsound.c i_sdlmusic.c)
DOOM_SRC=$(filter-out $(DOOM_EXCL) $(DOOM_EXCL_SND),$(wildcard $(DOOM_DIR)/*.c))
DOOM_OBJ=$(DOOM_SRC:.c=.o)
DOOM_CFLAGS=$(CFLAGS) -I$(DOOM_DIR) -include stddef.h -Wno-unused \
            -Wno-sign-compare -Wno-implicit-fallthrough -Wno-format -DNORMALUNIX

DESTDIR?=
ISODIR?=$(CURDIR)/../../isodir

PROGS=hello.elf help.elf calc.elf vix.elf diskinfo.elf lspci.elf maknetcfg.elf kbtester.elf makbox.elf makmux.elf statusbar.elf sigtest.elf maktop.elf clock.elf lines.elf forktest.elf execvetest.elf fdisk.elf cfdisk.elf basic.elf filetest.elf alloctest.elf sh.elf ar.elf microhash.elf sha256.elf md5.elf wget.elf ktest_uspace.elf gui.elf doom.elf files.elf mxterm.elf mxedit.elf mxfiles.elf mxtasks.elf

# libc.a -- single archive of the userspace shim (malloc + FILE* stdio +
# setjmp/longjmp + string/memory helpers).  Shipped under /usr/lib on the
# OS image so tcc(1) can link against it as -lc.  The objects are also
# linked individually into a few legacy app recipes (alloctest, filetest)
# below to give the archive passive in-guest-test coverage.
LIBC_OBJS=malloc.o stdio.o stdlib.o setjmp.o string.o tcc_compat.o dirent.o time.o

# tcc.elf is only built when the vendored snapshot at vendor/tinycc/ is
# populated (see build-tcc.sh).  The artefact lands here at the top of
# the userspace directory; if it's present, ship it.
TCC_ELF=$(wildcard tcc.elf)

# Headers shipped to /usr/include on the OS image so userland (and
# tcc-compiled programs) can resolve `#include <name.h>` directly.
SYSROOT_HEADERS=malloc.h stdio.h stdlib.h ctype.h setjmp.h string.h syscall.h dirent.h errno.h stdarg.h unistd.h time.h


all: $(PROGS) libc.a

hello.elf: crt0.o hello.o
	$(LD) $(LDFLAGS) -o $@ $^

help.elf: crt0.o help.o
	$(LD) $(LDFLAGS) -o $@ $^

calc.elf: crt0.o calc.o
	$(LD) $(LDFLAGS) -o $@ $^

sh.elf: crt0.o sh.o
	$(LD) $(LDFLAGS) -o $@ $^

microhash.elf: crt0.o microhash.o
	$(LD) $(LDFLAGS) -o $@ $^

sha256.elf: crt0.o sha256.o
	$(LD) $(LDFLAGS) -o $@ $^

md5.elf: crt0.o md5.o
	$(LD) $(LDFLAGS) -o $@ $^

wget.elf: crt0.o wget.o
	$(LD) $(LDFLAGS) -o $@ $^

ktest_uspace.elf: crt0.o ktest_uspace.o
	$(LD) $(LDFLAGS) -o $@ $^

vix.elf: crt0.o vix.o
	$(LD) $(LDFLAGS) -o $@ $^

gui.elf: crt0.o wm.o gui_gfx.o gui_ui.o gui_browser.o makx.o
	$(LD) $(LDFLAGS) -o $@ $^

# makx clients: each links the gui_gfx/gui_ui/gui_browser helpers it uses plus
# the makx client library (makx.o).  Applications are independent processes that
# talk to gui.elf (the display server) over IPC + a shared surface; see makx.h.
mxterm.elf: crt0.o mxterm.o gui_gfx.o makx.o
	$(LD) $(LDFLAGS) -o $@ $^

mxfiles.elf: crt0.o mxfiles.o gui_gfx.o gui_ui.o gui_browser.o makx.o
	$(LD) $(LDFLAGS) -o $@ $^

mxedit.elf: crt0.o mxedit.o gui_gfx.o gui_ui.o gui_browser.o makx.o
	$(LD) $(LDFLAGS) -o $@ $^

mxtasks.elf: crt0.o mxtasks.o gui_gfx.o gui_ui.o makx.o
	$(LD) $(LDFLAGS) -o $@ $^

files.elf: crt0.o files.o
	$(LD) $(LDFLAGS) -o $@ $^

# Vendored Doom objects: own include path + relaxed warnings.
$(DOOM_DIR)/%.o: $(DOOM_DIR)/%.c
	$(CC) $(DOOM_CFLAGS) -c -o $@ $<

doomgeneric_makar.o: doomgeneric_makar.c
	$(CC) $(DOOM_CFLAGS) -c -o $@ $<

# Link via the gcc driver so -lgcc (64-bit math helpers) resolves.  makx.o adds
# the (dormant unless `-makx` is passed) windowed makx-client backend; Doom's
# fullscreen text-mode path is unchanged.  No gui_gfx: the backend uses only the
# gfx_surface *type* from makx.h, not any drawing code.
doom.elf: crt0.o doomgeneric_makar.o $(DOOM_OBJ) makx.o libc.a
	$(CC) $(LDFLAGS) -o $@ crt0.o doomgeneric_makar.o $(DOOM_OBJ) makx.o libc.a -lgcc

ls.elf: crt0.o ls.o
	$(LD) $(LDFLAGS) -o $@ $^

diskinfo.elf: crt0.o diskinfo.o
	$(LD) $(LDFLAGS) -o $@ $^

lspci.elf: crt0.o lspci.o
	$(LD) $(LDFLAGS) -o $@ $^

maknetcfg.elf: crt0.o maknetcfg.o
	$(LD) $(LDFLAGS) -o $@ $^

makbox.elf: crt0.o makbox.o
	$(LD) $(LDFLAGS) -o $@ $^

makmux.elf: crt0.o makmux.o
	$(LD) $(LDFLAGS) -o $@ $^

statusbar.elf: crt0.o statusbar.o
	$(LD) $(LDFLAGS) -o $@ $^

kbtester.elf: crt0.o kbtester.o
	$(LD) $(LDFLAGS) -o $@ $^

sigtest.elf: crt0.o sigtest.o
	$(LD) $(LDFLAGS) -o $@ $^

maktop.elf: crt0.o maktop.o
	$(LD) $(LDFLAGS) -o $@ $^

clock.elf: crt0.o clock.o
	$(LD) $(LDFLAGS) -o $@ $^

lines.elf: crt0.o lines.o
	$(LD) $(LDFLAGS) -o $@ $^

forktest.elf: crt0.o forktest.o
	$(LD) $(LDFLAGS) -o $@ $^

execvetest.elf: crt0.o execvetest.o
	$(LD) $(LDFLAGS) -o $@ $^

fdisk.elf: crt0.o fdisk.o
	$(LD) $(LDFLAGS) -o $@ $^

cfdisk.elf: crt0.o cfdisk.o
	$(LD) $(LDFLAGS) -o $@ $^

basic.elf: crt0.o basic.o
	$(LD) $(LDFLAGS) -o $@ $^

filetest.elf: crt0.o filetest.o libc.a
	$(LD) $(LDFLAGS) -o $@ crt0.o filetest.o libc.a

alloctest.elf: crt0.o alloctest.o libc.a
	$(LD) $(LDFLAGS) -o $@ crt0.o alloctest.o libc.a

ar.elf: crt0.o ar.o libc.a
	$(LD) $(LDFLAGS) -o $@ crt0.o ar.o libc.a

# Archive of the userspace libc shim.  Built with rcs (replace, create
# index) so successive builds replace stale objects in place.  Debug
# sections are stripped from the archive because in-OS TCC trips over
# the cross-compiler's '.rel.debug_macro' relocations when loading
# libc.a -- TCC's reloc processor rejects entries whose debug-only
# symbol references resolve to slot 0 ('Invalid relocation entry
# [13]').  The standalone .o files in this directory keep their debug
# info for normal LD-driven builds; we only strip the archived copies.
libc.a: $(LIBC_OBJS)
	$(AR) rcs $@ $^
	$(OBJCOPY) --strip-debug $@


%.o: %.c
	$(CC) $(CFLAGS) -MMD -c -o $@ $<

%.o: %.S
	$(AS) --32 -o $@ $<

# Header-dependency tracking: -MMD emits a .d per object so a change to a
# shared header (e.g. syscall.h) recompiles every dependent.  Without this a
# syscall renumber silently left stale .elf calling the old number.
-include $(wildcard *.d)

install: all
	mkdir -p $(ISODIR)/apps
	rm -f $(ISODIR)/apps/ls.elf $(ISODIR)/apps/cp.elf $(ISODIR)/apps/mv.elf \
	      $(ISODIR)/apps/rm.elf $(ISODIR)/apps/echo.elf $(ISODIR)/apps/fsutil.elf
	cp $(PROGS) $(ISODIR)/apps/
	cp demo.sh incore.sh rebuild-kernel.sh kend.S getwad.sh $(ISODIR)/apps/
	-cp ../../wads/*.WAD ../../wads/*.wad $(ISODIR)/apps/ 2>/dev/null || true
	cp *.bas $(ISODIR)/apps/
# /usr sysroot for the in-OS compiler (tcc) and any future hosted ports.
# `crt0.o` is the C-runtime startup; `libc.a` the freestanding libc shim
# archive; headers under /usr/include resolve #include <name.h>; and the
# example sources under /usr/share/examples ship reference programs the
# in-guest test drivers can compile in-place.
	mkdir -p $(ISODIR)/usr/lib $(ISODIR)/usr/include $(ISODIR)/usr/share/examples
	cp crt0.o libc.a $(ISODIR)/usr/lib/
	cp $(SYSROOT_HEADERS) $(ISODIR)/usr/include/
	cp hello-tcc.c $(ISODIR)/usr/share/examples/
# tcc.elf rides along with the rest of the apps so PATH lookup resolves
# `tcc` from the kernel shell.  Only present when build-tcc.sh has been
# run with vendor/tinycc/ populated.
ifneq ($(TCC_ELF),)
	cp $(TCC_ELF) $(ISODIR)/apps/
endif

install-headers:

clean:
	rm -f *.o *.d $(PROGS) *.elf libc.a

.PHONY: all install install-headers clean

-include $(wildcard *.d)
