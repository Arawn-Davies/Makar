/*
 * keyboard.c -- PS/2 keyboard driver (layered, SMP-ready).
 *
 *   IRQ1 byte  -->  [decoder]      scancode set 1 + e0/e1 prefixes,
 *                                  strict make/break separation.
 *               -->  [translator]  keycode + modifier state -> cooked byte
 *                                  (ASCII printable, ASCII control, or one
 *                                   of the KEY_* sentinel bytes 0x80..0x88).
 *               -->  [router]      per-task SPSC ring (with proper memory
 *                                  ordering) or the global fallback ring.
 *
 * Public API (kernel/keyboard.h) is unchanged -- this is a drop-in replacement
 * for the previous monolithic handler.
 *
 * ===========================================================================
 * 1. WHY THIS REWRITE EXISTS
 * ===========================================================================
 *
 * The prior driver had three classes of latent bug, each of which has been
 * directly observed in production (see PR #123 thread):
 *
 *   1. "Sticky e0" -- the extended_key flag was a single bit set on every
 *      0xE0 byte and cleared only by the next non-prefix byte. If a byte was
 *      ever dropped (typematic burst, lost EOI under load, an emulator
 *      hiccup) the bit persisted and silently corrupted the *next* normal
 *      scancode -- e.g. a plain 'a' arriving in DEC_AFTER_E0 would surface
 *      as KEY_ARROW_<something>. The new decoder is a four-state machine,
 *      and an unexpected prefix byte cleanly restarts the relevant prefix
 *      instead of poisoning what follows.
 *
 *   2. SPSC ring race -- the IRQ producer wrote `slot->buf[head]` and then
 *      incremented `head` with no barrier between the two stores. On -O2
 *      the compiler is free to reorder, and a consumer that observed the
 *      new head before the new byte landed would read stale ring memory.
 *      That is the most likely cause of the "sporadic single-character noise
 *      not correlated to keystrokes" symptom flagged in CLAUDE.md slice #5.
 *      Producer and consumer now bracket every data/head pair with
 *      smp_wmb()/smp_rmb() so neither the compiler nor a future SMP runtime
 *      can shuffle the ordering.
 *
 *   3. Slot-table tearing -- the old kb_find_or_register() mutated the slot
 *      array and kb_nslots from task context with no synchronisation. An
 *      IRQ1 firing mid-mutation would walk a partial table and either
 *      dereference garbage or route to the wrong task. The new design is
 *      lock-free and uses a fixed-slot scheme (slots are never compacted;
 *      owner==NULL means free). All updates go through atomic builtins so
 *      the IRQ side always observes a consistent snapshot, and cross-CPU
 *      registration races are resolved by a single CAS.
 *
 * ===========================================================================
 * 2. UNSIGNED CHAR END-TO-END
 * ===========================================================================
 *
 * Internal types are uint8_t / unsigned char throughout the pipeline. The
 * sentinel keycodes (KEY_ARROW_UP and friends in keyboard.h) live in
 * 0x80..0x88. When stored as a signed `char` and widened to `int` they
 * sign-extend to 0xFFFFFF80.., which is fatal to any comparison that goes
 * via `int` -- e.g. `c >= 0x80` evaluates to false for a signed char with
 * value (char)0x80 because it becomes -128 < 128. As long as both operands
 * stay `char`, equality survives the widening; that's why the public API
 * still returns `char`. But the entire *producer* pipeline is unsigned, and
 * the conversion happens at exactly one place: the keyboard_{getchar,poll}
 * return path.
 *
 * ===========================================================================
 * 3. MEMORY ORDERING / SMP READINESS
 * ===========================================================================
 *
 * Makar is currently UP, but the design is SMP-correct so we do not have to
 * revisit this driver when SMP lands. The relevant facts:
 *
 *   - x86 (and x86_64) is TSO. Plain aligned scalar loads and stores up to
 *     pointer width are atomic, and the only reordering hardware will do is
 *     StoreLoad (a load may be observed before an earlier store to a
 *     different address). Inside the SPSC ring's data/head pair, both
 *     ordering directions we care about are StoreStore (producer) and
 *     LoadLoad (consumer), neither of which x86 reorders. So smp_wmb() and
 *     smp_rmb() collapse to compiler-only barriers on this architecture --
 *     same as Linux's barrier.h on i386.
 *
 *   - Cross-CPU full barriers (smp_mb()) require LOCK or MFENCE. We do not
 *     need one anywhere in the data path. The atomic builtins used for
 *     pointer publication are already SEQ_CST on x86 by the way GCC emits
 *     them on aligned stores.
 *
 *   - The IRQ handler can in principle run concurrently on two CPUs in the
 *     future (per-CPU local APIC vectors, or one CPU servicing keyboard
 *     while another sends to a task). The kb_io_lock spinlock serialises
 *     the controller drain so two CPUs can't tear the PS/2 byte stream.
 *     It is irq-safe (cli before acquire, restore on release) and on UP
 *     reduces to a no-op contention path.
 *
 * ===========================================================================
 * 4. CONTROLLER HYGIENE
 * ===========================================================================
 *
 * On every IRQ we read the status register first, drain up to a bounded
 * number of bytes from the data port, and skip mouse bytes (AUXB=1). We
 * filter the controller's own response codes (0x00 buffer-error, 0xAA BAT-
 * pass, 0xFA ack, 0xFE resend, 0xFF buffer-overflow) defensively. The
 * decoder never blocks; if the controller wedges, we ack what we have and
 * return.
 */

#include <kernel/keyboard.h>
#include <kernel/vtty.h>
#include <kernel/isr.h>
#include <kernel/asm.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <kernel/vesa_tty.h>

/* ===========================================================================
 * Memory-ordering primitives
 *
 * Modeled after Linux's <asm/barrier.h>. On x86 / x86_64 TSO collapses both
 * smp_wmb and smp_rmb to compiler-only barriers; smp_mb requires MFENCE.
 * Keep these here (not in a global header) until we have a real arch barrier
 * shim -- they need to be visible to the compiler in this translation unit
 * to do anything useful.
 * ======================================================================== */

#define barrier()       asm volatile("" ::: "memory")
#define smp_wmb()       barrier()           /* StoreStore: free on x86 TSO */
#define smp_rmb()       barrier()           /* LoadLoad : free on x86 TSO */
#define smp_mb()        asm volatile("mfence" ::: "memory")

/* WRITE_ONCE / READ_ONCE: tell the compiler to emit a single, non-tearing
 * load or store and not to fold the access into surrounding code. Modeled
 * on Linux's <linux/compiler.h>. We use these for any field that is read
 * by one context and written by another. */
#define READ_ONCE(x)        (*(const volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v)    (*(volatile __typeof__(x) *)&(x) = (v))

/* ===========================================================================
 * Spinlock (IRQ-safe). Single-CPU correctness is trivial; on SMP this is a
 * test-and-test-and-set spinlock with PAUSE in the back-off path.
 *
 * The "irqsave" variants save and disable EFLAGS.IF before acquiring so the
 * lock can't deadlock against itself when an IRQ on the same CPU tries to
 * acquire while the local CPU already holds it.
 * ======================================================================== */

typedef struct { volatile uint32_t locked; } kb_spinlock_t;

/*
 * kb_local_irq_save - atomically read EFLAGS and disable interrupts.
 *
 * Returns the previous EFLAGS so the caller can restore the IF state via
 * kb_local_irq_restore() without trampling on flags the rest of the kernel
 * may rely on (DF, AC, etc.). Equivalent to Linux's local_irq_save() on i386.
 *
 * The pushfl/popl/cli sequence is not strictly atomic at the instruction
 * boundary but is atomic with respect to interrupt delivery on this CPU --
 * the cli takes effect before the next IRQ check.
 */
static inline uint32_t kb_local_irq_save(void)
{
    uint32_t flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

/*
 * kb_local_irq_restore - restore EFLAGS (re-enables interrupts iff the
 * caller's flags had IF set when they called kb_local_irq_save).
 */
static inline void kb_local_irq_restore(uint32_t flags)
{
    asm volatile("pushl %0; popfl" :: "r"(flags) : "memory", "cc");
}

/*
 * kb_spin_lock - acquire the spinlock, blocking with PAUSE until it is free.
 *
 * Implemented as a test-and-test-and-set: we first spin on a relaxed load
 * (cheap, stays in the local cache line as a Shared read), and only attempt
 * the atomic exchange when the lock looks free. This avoids hammering the
 * cache line with RMW operations under contention. Acquire ordering on the
 * successful exchange ensures all subsequent loads/stores in the critical
 * section see the previous holder's released stores.
 */
static inline void kb_spin_lock(kb_spinlock_t *l)
{
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE) != 0) {
        while (READ_ONCE(l->locked) != 0)
            asm volatile("pause");
    }
}

/*
 * kb_spin_unlock - release the spinlock with release ordering so all stores
 * inside the critical section are visible to the next acquirer.
 */
static inline void kb_spin_unlock(kb_spinlock_t *l)
{
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

/*
 * kb_spin_lock_irqsave - kb_spin_lock + local IRQ disable, in that order.
 *
 * IRQs are disabled *before* the spin so the IRQ handler on the same CPU
 * can't fire while we're already holding the lock and try to take it again
 * (which would deadlock on UP and is a use-after-spin hazard on SMP).
 *
 * Returns EFLAGS as captured before disabling, for kb_spin_unlock_irqrestore.
 */
static inline uint32_t kb_spin_lock_irqsave(kb_spinlock_t *l)
{
    uint32_t flags = kb_local_irq_save();
    kb_spin_lock(l);
    return flags;
}

/*
 * kb_spin_unlock_irqrestore - mirror of kb_spin_lock_irqsave: release the
 * lock first, then restore the caller's previous IRQ-enable state.
 */
static inline void kb_spin_unlock_irqrestore(kb_spinlock_t *l, uint32_t flags)
{
    kb_spin_unlock(l);
    kb_local_irq_restore(flags);
}

/* Two locks. Splitting them lets the IRQ drain (kb_io_lock) run without
 * blocking task-side slot registration if the contention pattern ever
 * matters. For now both locks are held briefly and the split is just hygiene. */
static kb_spinlock_t kb_io_lock    = { 0 };  /* serialises controller drain */
static kb_spinlock_t kb_slots_lock = { 0 };  /* serialises slot table mutations */

/* ===========================================================================
 * PS/2 controller register layout
 * ======================================================================== */

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64

#define PS2_STAT_OBF    0x01    /* output buffer full -- byte ready in 0x60 */
#define PS2_STAT_AUXB   0x20    /* byte is from the AUX channel (mouse) */

/* ===========================================================================
 * Internal keycodes (decoder output, translator input).
 *
 * Encoding:
 *   0x00..0x7F  -- raw set-1 make scancode (single byte, no e0 prefix)
 *   0x80..0xFF  -- "extended" key, encoded as KC_EXT(post_e0_byte_low_7bits)
 *                  i.e. the keycode value that would have appeared in the
 *                  set-1 make byte if the e0 prefix could be inlined.
 *
 * The make/break bit is *not* part of the keycode -- it travels alongside
 * as a separate boolean so callers don't have to re-strip it.
 * ======================================================================== */

typedef uint8_t kc_t;

#define KC_ESC          0x01
#define KC_BACKSPACE    0x0E
#define KC_TAB          0x0F
#define KC_ENTER        0x1C
#define KC_LCTRL        0x1D
#define KC_LSHIFT       0x2A
#define KC_RSHIFT       0x36
#define KC_LALT         0x38
#define KC_SPACE        0x39
#define KC_CAPSLOCK     0x3A
#define KC_F1           0x3B
#define KC_F2           0x3C
#define KC_F3           0x3D
#define KC_F4           0x3E
#define KC_F5           0x3F
#define KC_F6           0x40
#define KC_F7           0x41
#define KC_F8           0x42
#define KC_F9           0x43
#define KC_F10          0x44
#define KC_F11          0x57
#define KC_F12          0x58

#define KC_EXT(b)       ((kc_t)((b) | 0x80))
#define KC_RCTRL        KC_EXT(0x1D)   /* e0 1d */
#define KC_RALT         KC_EXT(0x38)   /* e0 38 (AltGr) */
#define KC_KP_ENTER     KC_EXT(0x1C)
#define KC_KP_SLASH     KC_EXT(0x35)
#define KC_HOME         KC_EXT(0x47)
#define KC_END          KC_EXT(0x4F)
#define KC_PGUP         KC_EXT(0x49)
#define KC_PGDN         KC_EXT(0x51)
#define KC_INSERT       KC_EXT(0x52)
#define KC_DELETE       KC_EXT(0x53)
#define KC_ARROW_UP     KC_EXT(0x48)
#define KC_ARROW_DOWN   KC_EXT(0x50)
#define KC_ARROW_LEFT   KC_EXT(0x4B)
#define KC_ARROW_RIGHT  KC_EXT(0x4D)

/* ===========================================================================
 * Set-1 -> ASCII translation tables (US QWERTY).
 *
 * Indexed by raw single-byte make code (the 0x00..0x58 subset). Anything
 * outside that range translates to 0 (drop). Capital letters and shifted
 * symbols come from the second table.
 * ======================================================================== */

static const unsigned char kc_ascii_lower[89] = {
    0,    0,   '1', '2', '3', '4', '5', '6',
    '7', '8',  '9', '0', '-', '=', '\b', '\t',
    'q', 'w',  'e', 'r', 't', 'y', 'u', 'i',
    'o', 'p',  '[', ']', '\n', 0,  'a', 's',
    'd', 'f',  'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`', 0,  '\\','z', 'x', 'c', 'v',
    'b', 'n',  'm', ',', '.', '/', 0,   '*',
    0,   ' ',  0,   0,   0,   0,   0,   0,
    0,   0,    0,   0,   0,   0,   0,   '7',
    '8', '9',  '-', '4', '5', '6', '+', '1',
    '2', '3',  '0', '.', 0,   0,   0,   0,
    0
};

static const unsigned char kc_ascii_upper[89] = {
    0,    0,   '!', '@', '#', '$', '%', '^',
    '&', '*',  '(', ')', '_', '+', '\b', '\t',
    'Q', 'W',  'E', 'R', 'T', 'Y', 'U', 'I',
    'O', 'P',  '{', '}', '\n', 0,  'A', 'S',
    'D', 'F',  'G', 'H', 'J', 'K', 'L', ':',
    '"', '~',  0,   '|', 'Z', 'X', 'C', 'V',
    'B', 'N',  'M', '<', '>', '?', 0,   '*',
    0,   ' ',  0,   0,   0,   0,   0,   0,
    0,   0,    0,   0,   0,   0,   0,   '7',
    '8', '9',  '-', '4', '5', '6', '+', '1',
    '2', '3',  '0', '.', 0,   0,   0,   0,
    0
};

/* ===========================================================================
 * Ring buffers: SPSC, byte-wide, power-of-two size, 8-bit head/tail counters.
 *
 * Producer (IRQ): write data slot -> smp_wmb() -> publish new head.
 * Consumer (task): load head -> smp_rmb() -> read data slot -> advance tail.
 *
 * The buffer is "empty" when head == tail; "full" when (head - tail) ==
 * SIZE - 1. We keep one slot reserved so the empty/full states are
 * distinguishable. Counter wraparound is intentional and exact because the
 * sizes are powers of two: subtracting tail from head modulo 256 gives the
 * occupancy.
 *
 * On x86 a single-byte aligned store/load is atomic at the hardware level,
 * so head/tail observers never see torn values. The volatile qualifiers
 * defeat compiler hoisting in polling loops (e.g. keyboard_getchar's spin).
 * ======================================================================== */

#define KB_BUF_SIZE 256                  /* global fallback ring */
#define KB_BUF_MASK (KB_BUF_SIZE - 1)

static volatile uint8_t kb_buf[KB_BUF_SIZE];
static volatile uint8_t kb_buf_head = 0;
static volatile uint8_t kb_buf_tail = 0;

/*
 * buf_count_v - return the number of bytes currently queued in the global
 * fallback ring. Wraparound-safe because head and tail are uint8_t and the
 * buffer size is exactly 256: (head - tail) modulo 256 is the occupancy.
 */
static inline uint8_t buf_count_v(void)
{
    return (uint8_t)(READ_ONCE(kb_buf_head) - READ_ONCE(kb_buf_tail));
}

/*
 * buf_push - append c to the global fallback ring (SPSC, IRQ-side producer).
 *
 * Drops c silently if the ring is full -- losing input under sustained
 * overflow is preferable to blocking in IRQ context. The store-data,
 * smp_wmb(), publish-head sequence is the SPSC publication protocol that
 * pairs with buf_pop()'s load-head, smp_rmb(), read-data sequence.
 */
static void buf_push(unsigned char c)
{
    uint8_t head = kb_buf_head;
    uint8_t tail = READ_ONCE(kb_buf_tail);
    if ((uint8_t)(head - tail) >= KB_BUF_SIZE - 1) return;
    kb_buf[head & KB_BUF_MASK] = c;
    smp_wmb();                                                /* data before head */
    WRITE_ONCE(kb_buf_head, (uint8_t)(head + 1));
}

/*
 * buf_pop - remove and return the oldest byte in the global ring. Caller
 * must have established non-emptiness first (buf_count_v() != 0).
 *
 * The smp_rmb() between caller's head-observation and our data-read is what
 * stops the compiler from speculatively reading kb_buf[tail] before head is
 * proven greater than tail. The closing smp_wmb() before the tail bump is
 * symmetry hygiene -- on x86 it's a compiler-only fence.
 */
static unsigned char buf_pop(void)
{
    uint8_t tail = kb_buf_tail;
    smp_rmb();
    unsigned char c = kb_buf[tail & KB_BUF_MASK];
    smp_wmb();
    WRITE_ONCE(kb_buf_tail, (uint8_t)(tail + 1));
    return c;
}

#define KB_TASK_SLOTS  4
#define KB_SLOT_BUF    64
#define KB_SLOT_MASK   (KB_SLOT_BUF - 1)

typedef struct {
    /* owner pointer published with __atomic semantics. NULL = slot is free
     * and may be claimed via CAS. */
    task_t * volatile owner;
    volatile uint8_t  buf[KB_SLOT_BUF];
    volatile uint8_t  head;
    volatile uint8_t  tail;
} kb_slot_t;

static kb_slot_t kb_slots[KB_TASK_SLOTS];

/* The "focused" task receives all keyboard input. Pointer-sized so atomic
 * on x86; we use __atomic_{load,store}_n for explicit publication semantics. */
static task_t * volatile kb_focused = NULL;

/* Pane bindings (used by Ctrl-A,U / Ctrl-A,J shortcuts). */
static task_t * volatile kb_pane[2] = {NULL, NULL};

/* Ctrl-A prefix latch. Single byte, accessed only from IRQ context. */
static volatile uint8_t kb_prefix = 0;

/* SIGINT pending flag: set by the IRQ when it sees Ctrl+C, cleared by
 * keyboard_sigint_consume(). Atomic so the consumer can use a CAS-style
 * test-and-clear pattern. */
static volatile uint32_t kb_sigint = 0;

/*
 * slot_count_v - per-task ring occupancy. Same wraparound trick as
 * buf_count_v but on the 64-byte per-slot ring.
 */
static inline uint8_t slot_count_v(const kb_slot_t *s)
{
    return (uint8_t)(READ_ONCE(s->head) - READ_ONCE(s->tail));
}

/*
 * slot_empty_v - true iff the per-task ring is empty. Cheaper than
 * slot_count_v because it does not need a subtraction, just two loads.
 */
static inline int slot_empty_v(const kb_slot_t *s)
{
    return READ_ONCE(s->head) == READ_ONCE(s->tail);
}

/*
 * slot_push - push c into the per-task ring (SPSC, IRQ-side producer).
 *
 * Drops c if the ring is full. Same publication protocol as buf_push:
 * data store, smp_wmb(), head publish.
 */
static void slot_push(kb_slot_t *s, unsigned char c)
{
    uint8_t head = s->head;
    uint8_t tail = READ_ONCE(s->tail);
    if ((uint8_t)(head - tail) >= KB_SLOT_BUF - 1) return;
    s->buf[head & KB_SLOT_MASK] = c;
    smp_wmb();                                                /* data before head */
    WRITE_ONCE(s->head, (uint8_t)(head + 1));
}

/*
 * slot_pop - remove and return the oldest byte from the per-task ring.
 * Caller must have established non-emptiness first.
 */
static unsigned char slot_pop(kb_slot_t *s)
{
    uint8_t tail = s->tail;
    smp_rmb();                                                /* head before data */
    unsigned char c = s->buf[tail & KB_SLOT_MASK];
    smp_wmb();
    WRITE_ONCE(s->tail, (uint8_t)(tail + 1));
    return c;
}

/* ===========================================================================
 * Slot table: lock-free lookup, locked CAS-based mutation.
 *
 * Slots are never compacted -- once allocated, a task's slot index is stable
 * for the lifetime of that task. owner==NULL means the slot is free. The
 * IRQ-side scan ignores NULL owners. Task-side registration/release goes
 * through atomic CAS so two CPUs can race-claim a slot safely.
 * ======================================================================== */

/*
 * slot_lookup - find the slot index whose owner matches t, or -1 if t isn't
 * registered. Lock-free and safe from any context: owner pointers are
 * pointer-sized and aligned, so a single __atomic_load_n is atomic on x86,
 * and we never observe a torn pointer value.
 */
static int slot_lookup(task_t *t)
{
    for (int i = 0; i < KB_TASK_SLOTS; i++) {
        if (__atomic_load_n(&kb_slots[i].owner, __ATOMIC_ACQUIRE) == t)
            return i;
    }
    return -1;
}

/*
 * slot_register - register t into a free slot, or return its existing slot.
 * Returns the slot index, or -1 if the pool is exhausted.
 *
 * Fast path: a lock-free slot_lookup. If t is already registered we return
 * immediately without touching the lock.
 *
 * Slow path: take kb_slots_lock (irq-safe) and re-check under the lock --
 * a concurrent CPU may have raced us to register t. If still absent, scan
 * the table and CAS the first NULL owner from NULL to t. If we just claimed
 * the very first slot, install ourselves as the default keyboard focus so
 * that input has somewhere to go before vtty_register() runs.
 *
 * SMP-safe: even if two CPUs reach the slow path simultaneously, exactly
 * one CAS will succeed per free slot, and both will return a valid index
 * (or both will see "full" if the pool is exhausted).
 */
static int slot_register(task_t *t)
{
    if (!t) return -1;
    int existing = slot_lookup(t);
    if (existing >= 0) return existing;

    uint32_t flags = kb_spin_lock_irqsave(&kb_slots_lock);
    existing = slot_lookup(t);
    if (existing >= 0) {
        kb_spin_unlock_irqrestore(&kb_slots_lock, flags);
        return existing;
    }
    int idx = -1;
    for (int i = 0; i < KB_TASK_SLOTS; i++) {
        task_t *expected = NULL;
        if (__atomic_compare_exchange_n(&kb_slots[i].owner, &expected, t,
                                        /*weak=*/0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            kb_slots[i].head = 0;
            kb_slots[i].tail = 0;
            idx = i;
            break;
        }
    }
    if (idx >= 0 &&
        __atomic_load_n(&kb_focused, __ATOMIC_ACQUIRE) == NULL) {
        __atomic_store_n(&kb_focused, t, __ATOMIC_RELEASE);
    }
    kb_spin_unlock_irqrestore(&kb_slots_lock, flags);
    return idx;
}

/*
 * keyboard_release_task - free the slot bound to t and clear any focus or
 * pane bindings that point at it. Called when a task exits or relinquishes
 * its keyboard input (e.g. shell after `exec` returns).
 *
 * Order matters: clear focus and pane bindings *before* freeing the slot,
 * so the IRQ handler (which loads kb_focused with acquire ordering) cannot
 * route a byte to a slot that has already been NULL'd out.
 *
 * Idempotent and NULL-safe.
 */
void keyboard_release_task(task_t *t)
{
    if (!t) return;
    uint32_t flags = kb_spin_lock_irqsave(&kb_slots_lock);

    task_t *expected = t;
    __atomic_compare_exchange_n(&kb_focused, &expected, (task_t *)NULL,
                                /*weak=*/0,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    for (int p = 0; p < 2; p++) {
        expected = t;
        __atomic_compare_exchange_n(&kb_pane[p], &expected, (task_t *)NULL,
                                    /*weak=*/0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

    for (int i = 0; i < KB_TASK_SLOTS; i++) {
        if (__atomic_load_n(&kb_slots[i].owner, __ATOMIC_ACQUIRE) == t) {
            kb_slots[i].head = 0;
            kb_slots[i].tail = 0;
            __atomic_store_n(&kb_slots[i].owner, (task_t *)NULL,
                             __ATOMIC_RELEASE);
            break;
        }
    }
    kb_spin_unlock_irqrestore(&kb_slots_lock, flags);
}

/* ===========================================================================
 * Router
 * ======================================================================== */

/*
 * kb_route - deliver one cooked byte to the focused task, falling back to
 * the global ring if there is no focused task or no registered slot for it.
 *
 * Called from IRQ context only; it never mutates the slot table, never
 * blocks, and never recurses. The acquire load on kb_focused pairs with
 * the release stores in keyboard_set_focus() / keyboard_focus_pane() /
 * slot_register(), so we always see a fully-published owner pointer for
 * whatever task is currently focused.
 */
static void kb_route(unsigned char c)
{
    task_t *f = __atomic_load_n(&kb_focused, __ATOMIC_ACQUIRE);
    if (f) {
        for (int i = 0; i < KB_TASK_SLOTS; i++) {
            if (__atomic_load_n(&kb_slots[i].owner, __ATOMIC_ACQUIRE) == f) {
                slot_push(&kb_slots[i], c);
                return;
            }
        }
    }
    buf_push(c);
}

/* ===========================================================================
 * Decoder + modifier tracker
 *
 * State machine over raw set-1 bytes. Strict make/break separation: a break
 * (top bit set on the post-prefix byte) only updates modifier state and is
 * never fed to the translator. Auto-repeat is presented by the controller as
 * repeated *make* events, which is exactly what the shell wants for typematic
 * repeat -- we don't filter them.
 *
 * Lost-IRQ tolerance: the state machine is one-shot per prefix. An unexpected
 * 0xE0/0xE1 simply restarts the prefix; an unexpected non-prefix byte during
 * E1 advances or terminates the sequence. The decoder cannot livelock.
 * ======================================================================== */

typedef enum {
    DEC_NORMAL    = 0,
    DEC_AFTER_E0  = 1,
    DEC_AFTER_E1A = 2,
    DEC_AFTER_E1B = 3,
} dec_state_t;

static dec_state_t dec_state = DEC_NORMAL;

/* Modifier state lives at the decoder layer. We track left and right
 * sides separately so a "release-the-other-shift" sequence doesn't
 * accidentally clear a still-held shift. The compound mod_{shift,ctrl,alt}
 * fields are recomputed after every press/release. */
static int mod_shift   = 0;
static int mod_ctrl    = 0;
static int mod_alt     = 0;
static int mod_caps    = 0;
static int mod_lshift  = 0;
static int mod_rshift  = 0;
static int mod_lctrl   = 0;
static int mod_rctrl   = 0;
static int mod_lalt    = 0;
static int mod_ralt    = 0;

/*
 * translate_make - convert a keycode into the cooked byte that should be
 * routed to the focused task, taking current modifier state into account.
 * Returns 0 to mean "drop this event".
 *
 * Handles three classes of input:
 *
 *   1. Recognised extended keys (arrows, keypad enter/slash) become their
 *      KEY_* sentinel bytes or their plain ASCII equivalents.
 *   2. Other extended keys are dropped (we'll add Home/End/PgUp/PgDn/Del
 *      as the shell grows to use them).
 *   3. Single-byte make codes are looked up in the QWERTY ASCII table,
 *      with shift XOR caps governing letter case and shift alone governing
 *      symbol case. Ctrl + letter folds the result into the corresponding
 *      ASCII control code (Ctrl-A == 0x01, etc.).
 */
static unsigned char translate_make(kc_t kc)
{
    switch (kc) {
        case KC_ARROW_UP:    return (unsigned char)KEY_ARROW_UP;
        case KC_ARROW_DOWN:  return (unsigned char)KEY_ARROW_DOWN;
        case KC_ARROW_LEFT:  return (unsigned char)KEY_ARROW_LEFT;
        case KC_ARROW_RIGHT: return (unsigned char)KEY_ARROW_RIGHT;
        case KC_KP_ENTER:    return '\n';
        case KC_KP_SLASH:    return '/';
        default: break;
    }
    if (kc & 0x80) return 0;

    if (kc >= sizeof(kc_ascii_lower)) return 0;

    unsigned char lower = kc_ascii_lower[kc];
    if (!lower) return 0;

    int is_letter = (lower >= 'a' && lower <= 'z');
    int upper     = is_letter ? (mod_shift ^ mod_caps) : mod_shift;
    unsigned char c = upper ? kc_ascii_upper[kc] : lower;
    if (!c) return 0;

    if (mod_ctrl && is_letter)
        return (unsigned char)(lower - 'a' + 1);

    return c;
}

/*
 * apply_modifier - update modifier-key state on a make or break event.
 *
 * Tracks left and right sides of shift/ctrl/alt independently so that
 * holding LSHIFT, pressing and releasing RSHIFT, and continuing to hold
 * LSHIFT does not accidentally clear shift state in the middle. The
 * compound mod_shift / mod_ctrl / mod_alt fields are recomputed each time.
 *
 * Caps Lock is a *toggle* on press (down==1) and is otherwise unaffected
 * by the release event -- this matches every PC keyboard since the AT.
 *
 * Non-modifier keys are silently ignored.
 */
static void apply_modifier(kc_t kc, int is_break)
{
    int down = !is_break;
    switch (kc) {
        case KC_LSHIFT:   mod_lshift = down; break;
        case KC_RSHIFT:   mod_rshift = down; break;
        case KC_LCTRL:    mod_lctrl  = down; break;
        case KC_RCTRL:    mod_rctrl  = down; break;
        case KC_LALT:     mod_lalt   = down; break;
        case KC_RALT:     mod_ralt   = down; break;
        case KC_CAPSLOCK: if (down) mod_caps = !mod_caps; break;
        default: return;
    }
    mod_shift = mod_lshift | mod_rshift;
    mod_ctrl  = mod_lctrl  | mod_rctrl;
    mod_alt   = mod_lalt   | mod_ralt;
}

/*
 * on_make - dispatch a single "make" (key-down) event.
 *
 * Order of precedence (Linux's drivers/tty/vt/keyboard.c does roughly the
 * same thing in a much more sophisticated way):
 *
 *   1. Modifier keys never produce output.
 *   2. Alt + F1..F4 switches the active virtual TTY.
 *   3. Ctrl-A arms the pane prefix (consumed; never reaches the task).
 *   4. While the prefix is armed, the next make is interpreted as a pane
 *      command; anything else silently cancels the prefix.
 *   5. Ctrl-C raises the SIGINT flag and *also* delivers 0x03 to the
 *      focused task -- the shell readline observes the byte to abort the
 *      current input line, while shell_cmd_apps uses the flag to force-
 *      kill a child task during exec.
 *   6. Anything else is translated and routed.
 */
static void on_make(kc_t kc)
{
    switch (kc) {
        case KC_LSHIFT: case KC_RSHIFT:
        case KC_LCTRL:  case KC_RCTRL:
        case KC_LALT:   case KC_RALT:
        case KC_CAPSLOCK:
            return;
        default: break;
    }

    if (mod_alt) {
        switch (kc) {
            case KC_F1: vtty_switch(0); return;
            case KC_F2: vtty_switch(1); return;
            case KC_F3: vtty_switch(2); return;
            case KC_F4: vtty_switch(3); return;
            default: break;
        }
    }

    /* Resolve the unshifted ASCII letter once for the Ctrl-A / Ctrl-C
     * shortcuts; this avoids hardcoding scancode magic numbers. */
    unsigned char letter = (kc < sizeof(kc_ascii_lower)) ? kc_ascii_lower[kc] : 0;

    if (mod_ctrl && letter == 'a') {
        kb_prefix = 1;
        return;
    }

    if (kb_prefix) {
        kb_prefix = 0;
        if (letter == 'u')      keyboard_focus_pane(KB_PANE_TOP);
        else if (letter == 'j') keyboard_focus_pane(KB_PANE_BOTTOM);
        return;
    }

    if (mod_ctrl && letter == 'c') {
        __atomic_store_n(&kb_sigint, 1, __ATOMIC_RELEASE);
        kb_route(0x03);
        return;
    }

    unsigned char out = translate_make(kc);
    if (out) kb_route(out);
}

/*
 * decoder_feed - feed one raw set-1 byte to the decoder state machine.
 *
 * Output is exactly one keycode event (via apply_modifier + on_make) or
 * zero events, depending on whether the byte completes a keycode or merely
 * advances a prefix sequence.
 *
 * State transitions:
 *
 *   any state   + 0xE0          -> DEC_AFTER_E0   (start/restart e0 prefix)
 *   any state   + 0xE1          -> DEC_AFTER_E1A  (start/restart Pause seq)
 *   DEC_NORMAL  + b              -> emit kc=(b&0x7F), break=(b>>7)
 *   DEC_AFTER_E0+ b              -> emit kc=KC_EXT(b&0x7F), break=(b>>7);
 *                                   except (b&0x7F)==KC_LSHIFT (PrintScreen
 *                                   "fake shift") which is dropped to keep
 *                                   real shift state honest.
 *   DEC_AFTER_E1A + b            -> DEC_AFTER_E1B  (consume first half)
 *   DEC_AFTER_E1B + b            -> DEC_NORMAL     (consume second half)
 *
 * Controller-internal status bytes (0x00 buffer-error, 0xAA BAT pass,
 * 0xEE echo, 0xFA ack, 0xFE resend, 0xFF buffer-overflow) reset the state
 * machine to NORMAL without emitting an event -- they are responses to
 * controller commands and never represent keystrokes.
 */
static void decoder_feed(uint8_t sc)
{
    switch (sc) {
        case 0x00: case 0xAA: case 0xEE: case 0xFA: case 0xFE: case 0xFF:
            dec_state = DEC_NORMAL;
            return;
    }

    if (sc == 0xE0) { dec_state = DEC_AFTER_E0;  return; }
    if (sc == 0xE1) { dec_state = DEC_AFTER_E1A; return; }

    switch (dec_state) {

    case DEC_AFTER_E1A:
        dec_state = DEC_AFTER_E1B;
        return;

    case DEC_AFTER_E1B:
        dec_state = DEC_NORMAL;
        return;

    case DEC_AFTER_E0: {
        dec_state = DEC_NORMAL;
        int  is_break = (sc & 0x80) != 0;
        uint8_t low7  = sc & 0x7F;

        if (low7 == KC_LSHIFT) return;   /* PrintScreen fake-shift padding */

        kc_t kc = KC_EXT(low7);
        apply_modifier(kc, is_break);
        if (!is_break) on_make(kc);
        return;
    }

    case DEC_NORMAL:
    default: {
        int  is_break = (sc & 0x80) != 0;
        kc_t kc       = (kc_t)(sc & 0x7F);

        apply_modifier(kc, is_break);
        if (!is_break) on_make(kc);
        return;
    }
    }
}

/*
 * keyboard_irq_handler - IRQ1 service routine.
 *
 * Drains up to 16 bytes from the controller per IRQ. The status register is
 * checked first; if OBF is clear we stop early (the controller has nothing
 * left to give us). If AUXB is set the byte came from the mouse port -- we
 * still read it from PS2_DATA_PORT to ack it, but never feed it to the
 * decoder. The 16-byte cap prevents a runaway controller from livelocking
 * the kernel.
 *
 * SMP: kb_io_lock serialises the controller drain so a future second CPU
 * cannot interleave reads on 0x60 and produce a torn scancode stream. The
 * lock is irq-safe; on UP the spin path is never taken.
 */
static void keyboard_irq_handler(registers_t *regs)
{
    (void)regs;
    uint32_t flags = kb_spin_lock_irqsave(&kb_io_lock);

    for (int i = 0; i < 16; i++) {
        uint8_t status = inb(PS2_STATUS_PORT);
        if (!(status & PS2_STAT_OBF)) break;
        uint8_t sc = inb(PS2_DATA_PORT);
        if (status & PS2_STAT_AUXB) continue;
        decoder_feed(sc);
    }

    kb_spin_unlock_irqrestore(&kb_io_lock, flags);
}

/* ===========================================================================
 * Public API
 * ======================================================================== */

/*
 * keyboard_init - register the IRQ1 handler and drain any leftover bytes.
 *
 * Some controllers (or QEMU resets) leave a stray byte in the output buffer
 * before our handler is wired up; reading and discarding it ensures we
 * start with a clean slate and won't fire a spurious key on first IRQ.
 */
void keyboard_init(void)
{
    for (int i = 0; i < 16; i++) {
        uint8_t status = inb(PS2_STATUS_PORT);
        if (!(status & PS2_STAT_OBF)) break;
        (void)inb(PS2_DATA_PORT);
    }
    register_interrupt_handler(IRQ1, keyboard_irq_handler);
}

/*
 * keyboard_bind_pane - associate task t with pane_id (KB_PANE_TOP or
 * KB_PANE_BOTTOM) so that Ctrl-A,U / Ctrl-A,J can switch focus to it.
 *
 * Idempotently registers t in the slot table (so it has somewhere for
 * input to land) and then publishes the pane binding atomically.
 */
void keyboard_bind_pane(int pane_id, task_t *t)
{
    if (pane_id < 0 || pane_id > 1 || !t) return;
    slot_register(t);
    __atomic_store_n(&kb_pane[pane_id], t, __ATOMIC_RELEASE);
}

/*
 * keyboard_focus_pane - switch keyboard focus to the task currently bound
 * to pane_id, if any. No-op if the pane is unbound.
 */
void keyboard_focus_pane(int pane_id)
{
    if (pane_id < 0 || pane_id > 1) return;
    task_t *t = __atomic_load_n(&kb_pane[pane_id], __ATOMIC_ACQUIRE);
    if (t) __atomic_store_n(&kb_focused, t, __ATOMIC_RELEASE);
}

/*
 * keyboard_set_focus - direct-assign keyboard focus to t (used by
 * vtty_switch and shell_cmd_apps for explicit focus transitions). Passing
 * NULL means "no focused task; route to the global ring".
 */
void keyboard_set_focus(task_t *t)
{
    __atomic_store_n(&kb_focused, t, __ATOMIC_RELEASE);
}

/*
 * keyboard_sigint_consume - atomic test-and-clear of the SIGINT pending
 * flag. Returns 1 exactly once per Ctrl+C: even if multiple consumers
 * (shell readline, exec watcher) call this concurrently, exactly one will
 * see the 1, and the flag is cleared atomically.
 */
int keyboard_sigint_consume(void)
{
    uint32_t expected = 1;
    return __atomic_compare_exchange_n(&kb_sigint, &expected, 0,
                                       /*weak=*/0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/*
 * slot_for_current - resolve (and lazily register) the calling task's slot.
 * Returns -1 before tasking_init or if the slot pool is exhausted, in which
 * case the caller falls back to the global ring buffer.
 */
static int slot_for_current(void)
{
    task_t *me = task_current();
    if (!me) return -1;
    int s = slot_lookup(me);
    if (s >= 0) return s;
    return slot_register(me);
}

/*
 * keyboard_poll - non-blocking single-byte read. Returns 0 ('\0') if no
 * input is queued. Used by the shell at boot to drain stale input before
 * displaying the prompt, and by the exec wait loop to let SIGINT preempt
 * the busy-wait without blocking on input.
 */
char keyboard_poll(void)
{
    int s = slot_for_current();
    if (s >= 0) {
        kb_slot_t *slot = &kb_slots[s];
        if (slot_empty_v(slot)) return 0;
        return (char)slot_pop(slot);
    }
    if (buf_count_v() == 0) return 0;
    return (char)buf_pop();
}

/*
 * keyboard_send_to - inject one byte directly into t's input ring.
 *
 * Used by vtty_switch to deliver KEY_FOCUS_GAIN to the newly-focused task,
 * and could be used for synthetic input in tests. Lazily registers t if
 * needed. Safe to call from task or IRQ context (slot_register and
 * slot_push are both irq-safe).
 */
void keyboard_send_to(task_t *t, char c)
{
    if (!t) return;
    int s = slot_register(t);
    if (s >= 0) slot_push(&kb_slots[s], (unsigned char)c);
}

/*
 * keyboard_getchar - blocking single-byte read for the calling task.
 *
 * Cooperatively yields the CPU while waiting (task_yield) and emits a PAUSE
 * so we don't burn power in the spin. Returns the next cooked byte from the
 * task's slot ring, or from the global fallback ring if the task has no
 * registered slot (e.g. before tasking is up, or if the slot pool is full).
 */
char keyboard_getchar(void)
{
    int s = slot_for_current();
    if (s >= 0) {
        kb_slot_t *slot = &kb_slots[s];
        while (slot_empty_v(slot)) {
            vesa_tty_caret_blink_tick(timer_get_ticks());
            task_yield();
            asm volatile("pause");
        }
        return (char)slot_pop(slot);
    }
    while (buf_count_v() == 0) {
        vesa_tty_caret_blink_tick(timer_get_ticks());
        task_yield();
        asm volatile("pause");
    }
    return (char)buf_pop();
}
