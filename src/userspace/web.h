#ifndef _WEB_H
#define _WEB_H

/*
 * web.h -- a minimal HTTP/1.1 client, in userspace, over kernel TCP sockets.
 *
 * http:// rides a plain socket; https:// rides BearSSL (tls.h).  Follows 3xx
 * redirects (including http->https).  The kernel now owns only TCP/IP + sockets;
 * HTTP and TLS are application-layer and live here in ring 3.
 */

/* Fetch `url`, writing the response body to `outpath` (a writable VFS path,
 * e.g. under /tmp).  Returns the number of body bytes saved (>= 0), or:
 *   -1  DNS / connect / protocol error
 *   -2  write error (read-only path?)
 *   -(status)  for a final non-2xx HTTP status (after following redirects)
 */
int web_fetch(const char *url, const char *outpath);

#endif /* _WEB_H */
