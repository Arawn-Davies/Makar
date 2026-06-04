---
title: Networking
parent: Using Makar
nav_order: 4
---

# Networking

Makar currently has a monolithic lwIP bring-up path over the active kernel
`netdev` device. In the default QEMU test setup this is `virtio-net` connected
to QEMU user networking (`slirp`).

The interface is exposed to userspace as `eth0`.

## Virtual NICs

The kernel registers several PCI Ethernet backends and binds whichever QEMU
device is present:

| Driver | nettest selector | PCI ID |
|---|---|---|
| virtio-net legacy | `./run.sh nettest virtio` | `1AF4:1000` |
| RTL8139 | `./run.sh nettest rtl8139` | `10EC:8139` |
| Intel E1000/E1000E family baseline | `./run.sh nettest e1000` | `8086:100E`, `8086:100F` |
| AMD PCNet FAST | `./run.sh nettest pcnet` | `1022:2000` |

Each backend is a small polled driver feeding the same `netdev` contract:
`send`, `rx_poll`, and `mac`. Interrupt-driven RX/TX can come later without
changing lwIP's current attachment point.

## Networking test section

The networking suites (`netdev` ARP, `lwip_tcp`, `lwip_net_info`) live in their
own section, `ktest_run_net()`, dispatched by the `test=nettest` selector — they
are **not** part of the default `./run.sh ktest` gate, which stays NIC-agnostic.
`./run.sh nettest <nic>` builds the test ISO with `test_mode test=nettest`,
attaches the chosen QEMU NIC, and asserts on the serial marker:

```text
KTEST_NET_RESULT: PASS
```

The NIC is a positional argument (no environment variable needed); it defaults
to `virtio` when omitted. The same section validates every backend by re-running
it against a different NIC. RTL8139, E1000, and PCNet have each passed it against
QEMU slirp with ARP and lwIP TCP coverage.

The section also covers ICMP echo (`ping` to the live gateway, asserted; to
`1.1.1.1`, informational), DNS resolution (a dotted-quad literal asserted, a real
name informational), and `wget` (an HTTP GET against a slirp `guestfwd` HTTP
fixture).  Peer addresses are derived from the live netif, not hardcoded.

## DNS, ping, and HTTP (wget / unzip)

The lwIP stack exposes a small client surface:

- **DNS** — `net_lwip_resolve(host, ip, timeout)` resolves a hostname (or
  dotted-quad) via the lwIP resolver and the slirp-provided DNS server.
- **ICMP** — the kernel can send echo requests over the raw API (used by the
  net test section).
- **wget** — fetch a plain-HTTP URL and save it to the VFS. Available both as
  an in-kernel shell command and as `wget.elf` (via the `SYS_WGET` syscall,
  number 255):

  ```text
  wget http://host[:port]/path [outfile]      # outfile defaults to /tmp/<basename>
  ```

  Plain HTTP only — there is **no TLS**, so `https://` URLs are rejected. To
  pull from an HTTPS origin (e.g. GitHub), terminate TLS on the host and serve
  the file over plain HTTP; the guest reaches a host server at the slirp
  gateway (`10.0.2.2`) over TCP NAT, which streams multi-MB transfers fine
  (unlike a QEMU `guestfwd -cmd`, which caps response bodies near 160 KiB).

- **unzip** — `unzip <archive.zip> [destdir]` extracts a ZIP archive onto the
  VFS. A from-scratch RFC-1951 inflate (`fs/inflate.c`) handles stored and
  deflate entries; `fs/unzip.c` walks the central directory, creates parent
  directories, and rejects unsafe (`..`/absolute) names. Extract to a writable
  backend (`/tmp`, or a mounted ext2/FAT32 disk for persistence).

Example end-to-end (download + verify + extract), with a file served over plain
HTTP from a host/LAN server:

```text
wget http://10.0.0.111/main.zip /tmp/main.zip
cat /tmp/main.zip | md5            # compare against the published md5sum
unzip /tmp/main.zip /tmp/src
ls /tmp/src
```

## maknetcfg

`maknetcfg.elf` is the current network configuration tool:

```text
exec /apps/maknetcfg.elf
exec /apps/maknetcfg.elf release
exec /apps/maknetcfg.elf renew
exec /apps/maknetcfg.elf flush-dns
```

Without arguments it prints an `ipconfig`-style summary:

- active Ethernet driver
- link state
- MAC address
- DHCP state
- IPv4 address
- subnet mask
- default gateway
- DNS server

`release` stops the lwIP DHCP client and returns `eth0` to Makar's static QEMU
slirp fallback address:

```text
IPv4: 10.0.2.15/24
Gateway: 10.0.2.2
DNS: 10.0.2.3
```

`renew` restarts DHCP and waits briefly for a lease. If no lease arrives, Makar
keeps the same static slirp fallback so the interface remains usable in the
default virtualized setup.

`flush-dns` clears lwIP's DNS cache and pending resolver requests while keeping
the configured DNS server.

## Current Limits

Networking is still early:

- there is no POSIX socket API yet
- `ping`, `wget-lite`, `ftp`, and `telnet` are not implemented yet
- only one active `netdev` is selected
- DHCP is best-effort with deterministic static fallback for QEMU slirp
