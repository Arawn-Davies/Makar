#ifndef _HTML_H
#define _HTML_H

/*
 * html.h -- the renderer-independent core of Makar's HTML engine, shared by the
 * graphical browser (mxweb) and the text-mode browser (linx).  Layout/painting
 * is each browser's own concern; what they share is the *parser* surface:
 * HTML-entity decoding, tag-attribute extraction, and URL handling.  This is the
 * "reuse mxweb's engine" boundary -- the proven tokeniser primitives live here
 * once instead of being copied per browser.
 *
 * Freestanding: no libc dependency beyond the caller's buffers.
 */

/* Decode one HTML entity beginning at *pp (which must point at '&').  Writes up
 * to 3 bytes into out (give it >= 4), returns the byte count, and advances *pp
 * past the entity -- or past the lone '&' (returning a literal "&") if it isn't
 * a well-formed entity.  Handles &name; and &#dec;/&#xHEX; numeric refs, folding
 * common typographic codepoints (smart quotes, dashes, ellipsis) to ASCII. */
int html_entity(const char **pp, const char *end, char *out);

/* Find attribute `name` within a tag's attribute region [a,e) and copy its value
 * into out[cap] (NUL-terminated).  Returns 1 if found, 0 otherwise.  Accepts
 * "double", 'single', and bare (whitespace/`>`-terminated) values. */
int html_attr_get(const char *a, const char *e, const char *name, char *out, int cap);

/* 1 if `s` starts with a "scheme://" prefix (scheme before any '/'). */
int html_has_scheme(const char *s);

/* Split `u` into scheme / host / path.  `scheme` must hold >= 16 bytes, `host`
 * >= 256; `path` holds `pathcap` and always begins with '/'.  A schemeless input
 * yields empty scheme+host and path = the input (a relative/local path). */
void html_url_split(const char *u, char *scheme, char *host, char *path, int pathcap);

/* Resolve `ref` (an href/src, possibly relative) against `base` (the current
 * page URL) into out[cap].  Handles absolute, protocol-relative ("//host/x"),
 * root-relative ("/x") and same-directory ("x", "./x") references. */
void html_url_resolve(const char *base, const char *ref, char *out, int cap);

/* Normalise a user-typed address into out[cap]: prepend "http://" when there is
 * no scheme and it isn't a local absolute path, strip trailing whitespace, and
 * give a bare host a "/" path. */
void html_url_normalise(const char *in, char *out, int cap);

#endif /* _HTML_H */
