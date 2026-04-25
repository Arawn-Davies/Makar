#ifndef _KERNEL_KTEST_H
#define _KERNEL_KTEST_H

#include <kernel/tty.h>
#include <kernel/types.h>

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
 * ktest_run_all – run every registered test suite and print a summary.
 * Returns the total number of failed assertions (0 = all passed).
 * Called by the shell's `ktest` command and by TEST_MODE kernel_main.
 */
int ktest_run_all(void);

#endif /* _KERNEL_KTEST_H */
