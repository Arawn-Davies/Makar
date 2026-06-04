#!/bin/sh
# Minimal HTTP/1.1 origin server for the ktest net section.
# Wired to a QEMU slirp guestfwd (tcp:10.0.2.100:8080); when the guest
# connects, slirp runs this script with the socket on stdin/stdout.  We
# ignore the request and emit a fixed 200 response with a known body so
# `wget_fetch()` can be asserted deterministically, with no real internet.
body='MAKAR-WGET-OK'
printf 'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: %s\r\n\r\n%s' "${#body}" "$body"
