#ifndef _KERNEL_TIMER_H
#define _KERNEL_TIMER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <kernel/types.h>

void init_timer(uint32_t frequency);
uint32_t timer_get_ticks(void);
void ksleep(uint32_t ticks);

/*
 * Per-tick hook registry.  The timer IRQ stays display-agnostic: instead
 * of calling into tty/vesa_tty directly (a layering inversion), modules
 * that want a periodic callback register one here and the timer invokes
 * each per PIT tick with the current tick count.  Used for the boot
 * spinner and the status-bar clock.  Hooks run in IRQ context, so keep
 * them short.  Registration is one-way (no unregister); silently ignored
 * once the table is full.
 */
#define TIMER_MAX_TICK_HOOKS 8
typedef void (*timer_tick_fn)(uint32_t tick);
void timer_register_tick_hook(timer_tick_fn fn);

/* Preemptive scheduling quantum in PIT ticks (100 Hz).  Read by the
 * timer IRQ each tick; writable at runtime via the `sched_quantum`
 * shell builtin.  Bounds-check at the setter, not here. */
#define SCHED_QUANTUM_MIN 1u
#define SCHED_QUANTUM_MAX 100u
extern volatile uint32_t g_sched_quantum;

#endif