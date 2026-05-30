/*
 * build_origin.c -- where/what compiled this kernel.
 *
 * Three-way classification:
 *   "gcc-host"     -- normal ./run.sh iso build (cross-gcc on the dev host)
 *   "tcc-host"     -- ./build-kernel-tcc.sh on the dev host (host-side TCC)
 *   "tcc-in-os"    -- /apps/rebuild-kernel.sh inside a running Makar
 *
 * Routes:
 *   - gcc compiles this file with __TINYC__ undefined → "gcc-host".
 *   - TCC defines __TINYC__ automatically; the build path opts in to
 *     MAKAR_IN_OS_BUILD when it's the in-OS driver.
 *
 * Read at boot by kernel.c's banner.
 */

#ifdef __TINYC__
# ifdef MAKAR_IN_OS_BUILD
const char *MAKAR_BUILD_ORIGIN = "tcc-in-os";
# else
const char *MAKAR_BUILD_ORIGIN = "tcc-host";
# endif
#else
const char *MAKAR_BUILD_ORIGIN = "gcc-host";
#endif
