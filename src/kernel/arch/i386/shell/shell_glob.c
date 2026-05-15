/*
 * shell_glob.c -- wildcard (glob) expansion for the kernel shell.
 *
 * Expands `*` and `?` in argv tokens against the VFS via vfs_complete(),
 * so wildcards work uniformly across every mount: /hd (FAT32),
 * /cdrom (ISO9660), /proc (synthetic), and any future backend
 * (NFS/SMB/USB) that implements the same enumeration callback.
 *
 * Semantics:
 *   *      - matches zero or more chars, never crosses '/'
 *   ?      - matches exactly one char, never matches '/'
 *   no match - token is left unchanged (bash default, not zsh's NOMATCH=error)
 *   non-glob tokens - passed through untouched
 *
 * argv expansion is in-place: matches replace the wildcard token, and
 * subsequent argv entries shift right.  Capped at SHELL_MAX_ARGS total
 * so an over-broad glob never overruns the dispatch array.
 */

#include "shell_priv.h"

#include <kernel/vfs.h>
#include <kernel/fat32.h>   /* fat32_complete_cb_t */

#include <string.h>

/* ---------------------------------------------------------------------------
 * glob_has_wildcard - quick precheck so non-glob tokens skip the dir scan.
 * --------------------------------------------------------------------------- */
static int glob_has_wildcard(const char *s)
{
    for (; *s; s++)
        if (*s == '*' || *s == '?')
            return 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * glob_match - fnmatch-style match without FNM_PATHNAME complications.
 *
 * `*` greedy match over any chars; `?` exactly one.  Recursive form is fine
 * here because basenames are short and SHELL_MAX_INPUT caps the pattern.
 * --------------------------------------------------------------------------- */
static int glob_match(const char *pat, const char *name)
{
    while (*pat) {
        if (*pat == '*') {
            /* Collapse consecutive stars. */
            while (*pat == '*') pat++;
            if (!*pat) return 1;          /* trailing * matches the rest */
            while (*name) {
                if (glob_match(pat, name)) return 1;
                name++;
            }
            return 0;
        }
        if (!*name) return 0;
        if (*pat == '?' || *pat == *name) {
            pat++; name++;
            continue;
        }
        return 0;
    }
    return *name == '\0';
}

/* ---------------------------------------------------------------------------
 * Callback context: vfs_complete invokes glob_cb for every entry in the
 * scanned directory.  We filter by pattern, append the dir prefix, and
 * stash the resulting full path into the shared storage arena.
 * --------------------------------------------------------------------------- */
typedef struct {
    const char *pattern;     /* basename pattern (no slashes) */
    const char *dir_prefix;  /* "" if no dir was in the token, else "dir" or "dir/" */
    int         prefix_needs_slash;
    char       *buf;         /* next free byte in storage arena */
    size_t      buf_left;
    char      **matches;
    int         max_matches;
    int         nmatches;
} glob_ctx_t;

static void glob_cb(const char *name, int is_dir, void *opaque)
{
    (void)is_dir;
    glob_ctx_t *g = (glob_ctx_t *)opaque;

    if (g->nmatches >= g->max_matches) return;
    if (!glob_match(g->pattern, name))  return;

    /* Hide ".." entries unless the pattern explicitly asks for them - matches
     * shell convention and stops "ls *" from echoing parent-dir links. */
    if (name[0] == '.' && g->pattern[0] != '.')
        return;

    size_t plen = strlen(g->dir_prefix);
    size_t slen = g->prefix_needs_slash ? 1 : 0;
    size_t nlen = strlen(name);
    size_t need = plen + slen + nlen + 1;
    if (need > g->buf_left) return;

    char *out = g->buf;
    memcpy(out, g->dir_prefix, plen);
    if (slen) out[plen] = '/';
    memcpy(out + plen + slen, name, nlen + 1);

    g->matches[g->nmatches++] = out;
    g->buf      += need;
    g->buf_left -= need;
}

/* ---------------------------------------------------------------------------
 * shell_expand_globs - expand wildcards in argv in place.
 *
 *   argcp        : in/out token count
 *   argv         : in/out token pointers
 *   argv_cap     : max slots in argv (must be >= *argcp)
 *   storage      : scratch arena for expanded names
 *   storage_size : capacity of `storage`
 *
 * Tokens that contain `*` or `?` are split into dir + pattern, the dir is
 * enumerated via vfs_complete(), and matching basenames replace the token
 * (with the dir prefix prepended so the path is absolute when applicable).
 * Tokens with no matches are left untouched.  Returns the new argc.
 * --------------------------------------------------------------------------- */
int shell_expand_globs(int argc, char **argv, int argv_cap,
                       char *storage, size_t storage_size)
{
    char  *buf  = storage;
    size_t left = storage_size;

    int i = 0;
    while (i < argc) {
        char *tok = argv[i];
        if (!glob_has_wildcard(tok)) { i++; continue; }

        /* Split tok into dir_part and basename pattern. */
        char *last_slash = NULL;
        for (char *p = tok; *p; p++) if (*p == '/') last_slash = p;

        char dir_part[VFS_PATH_MAX];
        const char *pat;
        int   prefix_needs_slash;

        if (last_slash) {
            size_t dl = (size_t)(last_slash - tok);
            if (dl == 0) {
                /* Pattern was "/foo*" - scan root. */
                dir_part[0] = '/';
                dir_part[1] = '\0';
                prefix_needs_slash = 0;   /* "/" already ends in slash */
            } else {
                if (dl >= sizeof(dir_part)) dl = sizeof(dir_part) - 1;
                memcpy(dir_part, tok, dl);
                dir_part[dl] = '\0';
                prefix_needs_slash = 1;
            }
            pat = last_slash + 1;
        } else {
            /* No slash: scan CWD, emit basenames only. */
            dir_part[0] = '\0';
            prefix_needs_slash = 0;
            pat = tok;
        }

        if (!glob_has_wildcard(pat)) { i++; continue; }

        /* Enumerate and collect matches.  Cap per-token at the remaining argv
         * slots so we never need to re-truncate after the fact. */
        int    slots_left = argv_cap - argc;
        int    max_here   = slots_left + 1;  /* +1: the wildcard slot itself */
        if (max_here > 32) max_here = 32;

        char  *matches[32];
        glob_ctx_t ctx = {
            .pattern             = pat,
            .dir_prefix          = dir_part,
            .prefix_needs_slash  = prefix_needs_slash,
            .buf                 = buf,
            .buf_left            = left,
            .matches             = matches,
            .max_matches         = max_here,
            .nmatches            = 0,
        };

        const char *scan = (dir_part[0] == '\0') ? NULL : dir_part;
        vfs_complete(scan, "", glob_cb, &ctx);

        if (ctx.nmatches == 0) {
            /* No match - leave token alone (bash default behaviour). */
            i++;
            continue;
        }

        /* Reserve the consumed arena. */
        buf  = ctx.buf;
        left = ctx.buf_left;

        /* Splice matches into argv at position i, shifting the tail. */
        int extra = ctx.nmatches - 1;
        if (extra > 0) {
            /* Shift argv[i+1..argc-1] right by `extra`. */
            for (int j = argc - 1; j > i; j--)
                argv[j + extra] = argv[j];
        }
        for (int k = 0; k < ctx.nmatches; k++)
            argv[i + k] = matches[k];

        argc += extra;
        i    += ctx.nmatches;
    }

    return argc;
}
