#ifndef _KERNEL_SOCKET_H
#define _KERNEL_SOCKET_H

#include <stdint.h>

/*
 * socket.h -- in-kernel BSD-socket core (the kernel half of the userspace-TLS
 * move: the kernel owns the TCP/IP stack, userspace owns HTTP/TLS above it).
 *
 * A small fixed pool of TCP/IPv4 sockets over the lwIP raw API.  Every op runs
 * under the net big-lock and pumps lwIP itself (net_lwip_poll_ready) in its wait
 * loop -- the net_lwip_resolve() pattern -- so a ring-3 socket call and the net
 * task never re-enter NO_SYS lwIP concurrently.  Implemented in net/net_lwip.c
 * (co-located with the lock it must hold).
 *
 * Ring 3 reaches this via SYS_SOCKET / SYS_CONNECT; read()/write()/close() on
 * the FD_KIND_SOCKET fd route to ksock_recv/ksock_send/ksock_close.
 */

/* Allocate a socket from the pool.  Returns a socket id >= 0, or -1 if full. */
int  ksock_open(void);

/* Connect socket `id` to ip[4]:port (port in host byte order).  Blocks
 * (cooperatively, bounded by a connect timeout) until the handshake completes.
 * Returns 0 on success, -1 on failure. */
int  ksock_connect(int id, const uint8_t ip[4], uint16_t port);

/* Send `len` bytes on a connected socket.  Blocks (bounded) while the lwIP send
 * buffer drains.  Returns bytes written (> 0), or -1 on error. */
long ksock_send(int id, const void *buf, uint32_t len);

/* Receive up to `len` bytes.  Blocks (bounded by an idle timeout) for >= 1 byte.
 * Returns bytes read (> 0), 0 on a clean remote close (EOF), or -1 on error. */
long ksock_recv(int id, void *buf, uint32_t len);

/* Close + free socket `id`.  Idempotent; safe on an already-freed slot. */
int  ksock_close(int id);

#endif /* _KERNEL_SOCKET_H */
