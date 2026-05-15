#ifndef _KERNEL_KTEST_H
#define _KERNEL_KTEST_H

#include <kernel/tty.h>
#include <kernel/types.h>
#include <kernel/debug.h>

/*
 * Minimal in-kernel unit-test harness.
 *
 * Usage:
 *   ktest_begin("suite name");
 *   KTEST_ASSERT(expr);          // fails if expr is false
 *   KTEST_ASSERT_EQ(a, b);       // fails if a != b
 *   ktest_summary();             // prints pass/fail counts
 *
 * All macros write directly to the VGA terminal so they work before the heap
 * and before any subsystem other than the TTY is initialised.
 */

/* Internal state – defined in ktest.c. */
extern int ktest_pass_count;
extern int ktest_fail_count;

void ktest_begin(const char *suite);
void ktest_summary(void);
void ktest_assert(int cond, const char *expr, const char *file, uint32_t line);

#define KTEST_ASSERT(expr) \
    ktest_assert(!!(expr), #expr, __FILE__, __LINE__)

#define KTEST_ASSERT_EQ(a, b) \
    ktest_assert((a) == (b), #a " == " #b, __FILE__, __LINE__)

/*
 * KTEST_ASSERT_MAJOR – like KTEST_ASSERT but triggers kpanic_at on failure.
 * Use for assertions that indicate kernel corruption if false.
 */
#define KTEST_ASSERT_MAJOR(expr) \
    do { \
        ktest_assert(!!(expr), #expr, __FILE__, __LINE__); \
        if (!(expr)) kpanic_at("ktest major failure: " #expr, \
                                __FILE__, __func__, __LINE__); \
    } while (0)

/*
 * ktest_run_all – run every registered test suite and print a summary.
 * Returns the total number of failed assertions (0 = all passed).
 * Called by the shell's `ktest` command and by kernel_main when the
 * multiboot2 command line contains "test_mode".
 */
int ktest_run_all(void);

/*
 * ktest_bg_task – task entry for silent background ktest at boot.
 * Suppresses per-assertion VGA output; prints only on failure.
 * Sets ktest_bg_done = 1 when finished.
 */
void ktest_bg_task(void);

/* When non-zero, suppress VGA output from ktest and syscall handlers. */
extern int ktest_muted;

/* Set to 1 by ktest_bg_task when the background run completes. */
extern volatile int ktest_bg_done;

/* Progress counters driven by ktest_bg_task: ktest_bg_completed advances
 * as each background suite finishes; ktest_bg_total is the number of
 * suites that will run in the bg pass.  Used by the shell loading screen
 * to render a progress bar that fills as bg ktest makes progress. */
extern volatile int ktest_bg_completed;
extern const    int ktest_bg_total;

/* Empty hook called immediately after ktest_bg_done is set to 1.  The
 * iso-test GDB harness breakpoints here to confirm bg ktest finished,
 * decoupling the assertion from shell0's loading-screen wall clock. */
void ktest_bg_marker(void);

#endif /* _KERNEL_KTEST_H */
