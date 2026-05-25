/*
 * dirent.c -- POSIX opendir/readdir/closedir over SYS_READDIR(141).
 *
 * SYS_READDIR is stateless and index-addressed: each call passes the
 * directory path and an integer index, fills a struct dirent, and
 * returns 1 (filled) / 0 (end) / -1 (error).  These three wrappers
 * give it the standard POSIX shape so ported apps don't have to know
 * about the underlying call.
 */

#include "dirent.h"
#include "stdlib.h"     /* malloc/free */
#include "string.h"     /* strlen/memcpy */

DIR *opendir(const char *path)
{
    if (!path) return 0;
    unsigned int n = (unsigned int)strlen(path);
    if (n >= sizeof(((DIR *)0)->path)) return 0;

    /* Verify the path is readable now so callers don't get a NULL
     * readdir on a missing directory.  An empty directory legitimately
     * returns 0 from the first readdir call -- that's distinguishable
     * from this error case via the opendir return. */
    struct dirent probe;
    int r = sys_readdir(path, 0, &probe);
    if (r < 0) return 0;

    DIR *dp = (DIR *)malloc(sizeof(DIR));
    if (!dp) return 0;
    memcpy(dp->path, path, n + 1);
    dp->idx = 0;
    return dp;
}

struct dirent *readdir(DIR *dp)
{
    if (!dp) return 0;
    int r = sys_readdir(dp->path, dp->idx, &dp->de);
    if (r != 1) return 0;
    dp->idx++;
    return &dp->de;
}

int closedir(DIR *dp)
{
    if (dp) free(dp);
    return 0;
}
