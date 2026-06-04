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
