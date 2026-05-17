#ifndef _KERNEL_TIMER_H
#define _KERNEL_TIMER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <kernel/types.h>

void init_timer(uint32_t frequency);
uint32_t timer_get_ticks(void);
void ksleep(uint32_t ticks);

/* Preemptive scheduling quantum in PIT ticks (100 Hz).  Read by the
 * timer IRQ each tick; writable at runtime via the `sched_quantum`
 * shell builtin.  Bounds-check at the setter, not here. */
#define SCHED_QUANTUM_MIN 1u
#define SCHED_QUANTUM_MAX 100u
extern volatile uint32_t g_sched_quantum;

#endif