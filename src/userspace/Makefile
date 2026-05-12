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

PROGS=hello.elf echo.elf help.elf calc.elf vics.elf ls.elf diskinfo.elf rm.elf mv.elf cp.elf kbtester.elf


all: $(PROGS)

hello.elf: crt0.o hello.o
	$(LD) $(LDFLAGS) -o $@ $^

echo.elf: crt0.o echo.o
	$(LD) $(LDFLAGS) -o $@ $^

help.elf: crt0.o help.o
	$(LD) $(LDFLAGS) -o $@ $^

calc.elf: crt0.o calc.o
	$(LD) $(LDFLAGS) -o $@ $^

vics.elf: crt0.o vics.o
	$(LD) $(LDFLAGS) -o $@ $^

ls.elf: crt0.o ls.o
	$(LD) $(LDFLAGS) -o $@ $^

diskinfo.elf: crt0.o diskinfo.o
	$(LD) $(LDFLAGS) -o $@ $^

rm.elf: crt0.o rm.o
	$(LD) $(LDFLAGS) -o $@ $^

mv.elf: crt0.o mv.o
	$(LD) $(LDFLAGS) -o $@ $^

cp.elf: crt0.o cp.o
	$(LD) $(LDFLAGS) -o $@ $^

kbtester.elf: crt0.o kbtester.o
	$(LD) $(LDFLAGS) -o $@ $^


%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(AS) --32 -o $@ $<

install: all
	mkdir -p $(ISODIR)/apps
	cp $(PROGS) $(ISODIR)/apps/

install-headers:

clean:
	rm -f *.o $(PROGS) *.elf

.PHONY: all install install-headers clean
