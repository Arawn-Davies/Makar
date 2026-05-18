DEFAULT_HOST!=../default-host.sh
HOST?=$(DEFAULT_HOST)

CC=$(HOST)-gcc
AS=$(HOST)-as
LD=$(HOST)-ld

CFLAGS?=-O2 -g
CFLAGS:=$(CFLAGS) -std=c11 -ffreestanding -fno-stack-protector \
        -Wall -Wextra -nostdlib -nostdinc -I.

LDFLAGS=-T link.ld -nostdlib

DESTDIR?=
ISODIR?=$(CURDIR)/../../isodir

PROGS=hello.elf help.elf calc.elf vix.elf diskinfo.elf kbtester.elf makbox.elf sigtest.elf maktop.elf clock.elf


all: $(PROGS)

hello.elf: crt0.o hello.o
	$(LD) $(LDFLAGS) -o $@ $^

help.elf: crt0.o help.o
	$(LD) $(LDFLAGS) -o $@ $^

calc.elf: crt0.o calc.o
	$(LD) $(LDFLAGS) -o $@ $^

vix.elf: crt0.o vix.o
	$(LD) $(LDFLAGS) -o $@ $^

ls.elf: crt0.o ls.o
	$(LD) $(LDFLAGS) -o $@ $^

diskinfo.elf: crt0.o diskinfo.o
	$(LD) $(LDFLAGS) -o $@ $^

makbox.elf: crt0.o makbox.o
	$(LD) $(LDFLAGS) -o $@ $^

kbtester.elf: crt0.o kbtester.o
	$(LD) $(LDFLAGS) -o $@ $^

sigtest.elf: crt0.o sigtest.o
	$(LD) $(LDFLAGS) -o $@ $^

maktop.elf: crt0.o maktop.o
	$(LD) $(LDFLAGS) -o $@ $^

clock.elf: crt0.o clock.o
	$(LD) $(LDFLAGS) -o $@ $^


%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(AS) --32 -o $@ $<

install: all
	mkdir -p $(ISODIR)/apps
	rm -f $(ISODIR)/apps/ls.elf $(ISODIR)/apps/cp.elf $(ISODIR)/apps/mv.elf \
	      $(ISODIR)/apps/rm.elf $(ISODIR)/apps/echo.elf $(ISODIR)/apps/fsutil.elf
	cp $(PROGS) $(ISODIR)/apps/

install-headers:

clean:
	rm -f *.o $(PROGS) *.elf

.PHONY: all install install-headers clean
