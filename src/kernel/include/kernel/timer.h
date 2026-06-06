#ifndef _KERNEL_TIMER_H
#define _KERNEL_TIMER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <kernel/types.h>

/* PIT tick rate.  250 Hz = a 4 ms tick, giving the preemptive scheduler a
 * fine grain so the GUI compositor runs promptly between client time-slices
 * (the old 100 Hz / 40 ms slice made multi-window + DOOM feel laggy).  This
 * is the single source of truth for the rate; init_timer() is called with it
 * and all tick<->wall-clock conversions divide/multiply by it. */
#define TIMER_HZ 250u

void init_timer(uint32_t frequency);
uint32_t timer_get_ticks(void);

/* USER_HZ: the *userspace-facing* tick rate, fixed at 100 Hz regardless of the
 * internal TIMER_HZ -- exactly like Linux's stable USER_HZ / CLOCKS_PER_SEC.
 * SYS_UPTIME and the /proc CPU-tick counters report in these units so apps
 * that assume 100 ticks/second (clock widget's `+100u` = 1 s, maktop's refresh
 * cadence, CPU% ratios) stay correct after the rate change. Both the uptime
 * and the per-task kticks are scaled by the same factor, so their ratio (used
 * for CPU%) is preserved. */
#define USER_HZ 100u
static inline uint32_t timer_to_user_ticks(uint32_t real_ticks)
{
    return (uint32_t)(((uint64_t)real_ticks * USER_HZ) / TIMER_HZ);
}
static inline uint32_t timer_user_ticks(void)
{
    return timer_to_user_ticks(timer_get_ticks());
}

/* ksleep(): busy-wait for `ticks` *legacy 100 Hz units* (i.e. centiseconds,
 * 10 ms each), independent of the real TIMER_HZ.  Kept HZ-independent so the
 * many call sites that mean a wall-clock delay ("1 s pause" = ksleep(100))
 * keep their exact duration after the rate change -- the function scales the
 * argument to real PIT ticks internally. */
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

/* Preemptive scheduling quantum in PIT ticks (TIMER_HZ).  Read by the
 * timer IRQ each tick; writable at runtime via the `sched_quantum`
 * shell builtin.  Bounds-check at the setter, not here.  Default 1 tick
 * @ 250 Hz = a 4 ms base slice (scaled per task by task_t.sched_weight). */
#define SCHED_QUANTUM_MIN 1u
#define SCHED_QUANTUM_MAX 250u
extern volatile uint32_t g_sched_quantum;

#endif