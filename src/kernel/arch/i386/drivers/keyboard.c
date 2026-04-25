//
// keyboard.c -- PS/2 keyboard driver.
// Handles IRQ1, translates scan-code set 1 to ASCII (US QWERTY layout),
// tracks shift/caps-lock state, and provides a 256-byte ring buffer.
//

#include <kernel/keyboard.h>
#include <kernel/isr.h>
#include <kernel/asm.h>
#include <kernel/task.h>

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64

// Scan-code set 1: key-release codes have bit 7 set (scan | 0x80).
#define SC_LSHIFT       0x2A
#define SC_RSHIFT       0x36
#define SC_LSHIFT_REL   (SC_LSHIFT | 0x80)
#define SC_RSHIFT_REL   (SC_RSHIFT | 0x80)
#define SC_LCTRL        0x1D   /* Left Ctrl key (press)                  */
#define SC_LCTRL_REL    (SC_LCTRL | 0x80)
#define SC_CAPSLOCK     0x3A
#define SC_BACKSPACE    0x0E
#define SC_ENTER        0x1C

// ---------------------------------------------------------------------------
// Scan-code → ASCII tables (unshifted and shifted), indexed by scan code.
// Only scan-code set 1, codes 0x00-0x58 are mapped; everything else → 0.
// ---------------------------------------------------------------------------

static const char sc_ascii_lower[89] = {
    0,    0,   '1', '2', '3', '4', '5', '6',  // 0x00-0x07
    '7', '8',  '9', '0', '-', '=', '\b', '\t', // 0x08-0x0F
    'q', 'w',  'e', 'r', 't', 'y', 'u', 'i',  // 0x10-0x17
    'o', 'p',  '[', ']', '\n', 0,  'a', 's',  // 0x18-0x1F
    'd', 'f',  'g', 'h', 'j', 'k', 'l', ';',  // 0x20-0x27
    '\'', '`', 0,  '\\','z', 'x', 'c', 'v',   // 0x28-0x2F
    'b', 'n',  'm', ',', '.', '/', 0,   '*',   // 0x30-0x37
    0,   ' ',  0,   0,   0,   0,   0,   0,     // 0x38-0x3F
    0,   0,    0,   0,   0,   0,   0,   '7',   // 0x40-0x47
    '8', '9',  '-', '4', '5', '6', '+', '1',   // 0x48-0x4F
    '2', '3',  '0', '.', 0,   0,   0,   0,     // 0x50-0x57
    0                                           // 0x58
};

static const char sc_ascii_upper[89] = {
    0,    0,   '!', '@', '#', '$', '%', '^',   // 0x00-0x07
    '&', '*',  '(', ')', '_', '+', '\b', '\t', // 0x08-0x0F
    'Q', 'W',  'E', 'R', 'T', 'Y', 'U', 'I',  // 0x10-0x17
    'O', 'P',  '{', '}', '\n', 0,  'A', 'S',  // 0x18-0x1F
    'D', 'F',  'G', 'H', 'J', 'K', 'L', ':',  // 0x20-0x27
    '"', '~',  0,   '|', 'Z', 'X', 'C', 'V',  // 0x28-0x2F
    'B', 'N',  'M', '<', '>', '?', 0,   '*',   // 0x30-0x37
    0,   ' ',  0,   0,   0,   0,   0,   0,     // 0x38-0x3F
    0,   0,    0,   0,   0,   0,   0,   '7',   // 0x40-0x47
    '8', '9',  '-', '4', '5', '6', '+', '1',   // 0x48-0x4F
    '2', '3',  '0', '.', 0,   0,   0,   0,     // 0x50-0x57
    0                                           // 0x58
};

// ---------------------------------------------------------------------------
// 256-byte ring buffer
// ---------------------------------------------------------------------------

#define KB_BUF_SIZE 256

static volatile uint8_t kb_buf[KB_BUF_SIZE];
static volatile uint8_t kb_buf_head = 0; // next write position
static volatile uint8_t kb_buf_tail = 0; // next read position

// Returns number of characters currently in the buffer.
static inline uint8_t buf_count(void)
{
    return (uint8_t)(kb_buf_head - kb_buf_tail);
}

// Push one character into the ring buffer; silently drops on overflow.
static inline void buf_push(char c)
{
    if (buf_count() < KB_BUF_SIZE - 1) {
        kb_buf[kb_buf_head++] = (uint8_t)c;
    }
}

// Pop one character from the ring buffer (caller must ensure it is non-empty).
static inline char buf_pop(void)
{
    return (char)kb_buf[kb_buf_tail++];
}

// ---------------------------------------------------------------------------
// Modifier state
// ---------------------------------------------------------------------------

static volatile int shift_pressed  = 0;
static volatile int caps_lock_on   = 0;
static volatile int ctrl_pressed   = 0;  /* set while left Ctrl is held    */
static volatile int extended_key   = 0;  /* set when 0xE0 prefix is received */

// ---------------------------------------------------------------------------
// IRQ1 handler
// ---------------------------------------------------------------------------

static void keyboard_irq_handler(registers_t regs)
{
    (void)regs;

    uint8_t sc = inb(PS2_DATA_PORT);

    // The 0xE0 prefix introduces an extended (two-byte) scan code.
    // Set a flag and wait for the actual key code in the next interrupt.
    if (sc == 0xE0) {
        extended_key = 1;
        return;
    }

    // Handle extended key-press (ignore extended key-release, bit 7 set).
    if (extended_key) {
        extended_key = 0;
        if (!(sc & 0x80)) {
            if (sc == 0x48) { buf_push(KEY_ARROW_UP);    return; }
            if (sc == 0x50) { buf_push(KEY_ARROW_DOWN);  return; }
            if (sc == 0x4B) { buf_push(KEY_ARROW_LEFT);  return; }
            if (sc == 0x4D) { buf_push(KEY_ARROW_RIGHT); return; }
            // All other extended keys are silently ignored for now.
        }
        return;
    }

    // Handle modifier key releases.
    if (sc == SC_LSHIFT_REL || sc == SC_RSHIFT_REL) {
        shift_pressed = 0;
        return;
    }
    if (sc == SC_LCTRL_REL) {
        ctrl_pressed = 0;
        return;
    }

    // Ignore all other key-release scan codes (bit 7 set).
    if (sc & 0x80) {
        return;
    }

    // Handle modifier key presses.
    if (sc == SC_LSHIFT || sc == SC_RSHIFT) {
        shift_pressed = 1;
        return;
    }
    if (sc == SC_LCTRL) {
        ctrl_pressed = 1;
        return;
    }

    if (sc == SC_CAPSLOCK) {
        caps_lock_on = !caps_lock_on;
        return;
    }

    // Translate scan code to ASCII.
    if (sc >= sizeof(sc_ascii_lower)) {
        return;
    }

    // Determine the character: caps-lock XOR shift applies only to letters;
    // for all other keys, only shift determines the symbol.
    char lower_c = sc_ascii_lower[sc];
    int is_letter = (lower_c >= 'a' && lower_c <= 'z');
    int use_upper = is_letter ? (shift_pressed ^ caps_lock_on) : shift_pressed;
    char c = use_upper ? sc_ascii_upper[sc] : sc_ascii_lower[sc];

    if (c == 0) {
        return;
    }

    // When Ctrl is held and a letter key is pressed, generate a control
    // character (Ctrl+E = 0x05, Ctrl+Q = 0x11, Ctrl+S = 0x13, etc.).
    // Ctrl+A (0x01) through Ctrl+D (0x04) are skipped because those values
    // are already reserved for the arrow-key sentinels.
    if (ctrl_pressed && is_letter) {
        char ctrl_code = (char)(lower_c - 'a' + 1);  /* Ctrl+A=1, …, Ctrl+Z=26 */
        if (ctrl_code > 4) {   /* skip 0x01-0x04 (arrow key codes) */
            buf_push(ctrl_code);
        }
        return;
    }

    buf_push(c);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void keyboard_init(void)
{
    register_interrupt_handler(IRQ1, keyboard_irq_handler);
}

char keyboard_poll(void)
{
    if (buf_count() == 0) {
        return 0;
    }
    return buf_pop();
}

char keyboard_getchar(void)
{
    while (buf_count() == 0) {
        task_yield();              /* cooperatively yield while waiting for input */
        asm volatile("pause");    /* spin-loop hint to reduce bus traffic         */
    }
    return buf_pop();
}
