#ifndef _KERNEL_DEBUG_H
#define _KERNEL_DEBUG_H

/* Register GDB-aware handlers for INT 1/3 (debug/breakpoint) and
 * INT 8/13/14 (double-fault, GPF, page-fault). */
void init_debug_handlers(void);

/* Unconditional kernel panic - renders panic screen and halts. Never returns. */
void kpanic(const char *msg);

/* Like kpanic but records file, function, and line for the panic screen. */
void kpanic_at(const char *msg, const char *file, const char *func, int line);

/* Convenience macro: captures __FILE__, __func__, __LINE__ automatically. */
#define KPANIC(msg) kpanic_at((msg), __FILE__, __func__, __LINE__)

#endif /* _KERNEL_DEBUG_H */
