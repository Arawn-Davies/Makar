#ifndef _MAKAR_KEYS_H
#define _MAKAR_KEYS_H

/*
 * makar_keys.h -- canonical KEY_* input sentinels, shared by the kernel
 * (<kernel/keyboard.h>) and userspace (src/userspace/syscall.h) so the decode
 * pipeline and the apps that read sys_getkey() agree.  Was kept in sync by hand.
 * Cast as unsigned char so the value stays positive through integer promotion
 * (a plain (char)0x80 silently broke `unsigned char c == KEY_*` comparisons).
 * No types needed -- safe under the userspace -nostdinc build.
 */
/*
 * Sentinels for extended (non-ASCII) keys - high-byte range so they never
 * collide with any Ctrl+letter code (0x01-0x1A).
 *
 * Cast as `unsigned char` (not plain `char`) so the type stays positive
 * through integer promotion. A previous `((char)0x80)` form silently broke
 * any consumer that read into `unsigned char c` -- the comparison
 * `c == KEY_ARROW_RIGHT` widened c to int 0x83 and the macro to int -125,
 * never matched, and at high optimisation jump-table dispatch could land on
 * a wild address (EIP=0xFFFFFF83 observed on PR #124 / kbtester).
 */
#define KEY_ARROW_UP    ((unsigned char)0x80)
#define KEY_ARROW_DOWN  ((unsigned char)0x81)
#define KEY_ARROW_LEFT  ((unsigned char)0x82)
#define KEY_ARROW_RIGHT ((unsigned char)0x83)

/* Function key sentinels (Alt+F1-F4 trigger TTY switching).
 * F1..F12 occupy 0x84..0x87 (legacy F1-F4) and 0x89..0x90 (F5-F12). */
#define KEY_F1          ((unsigned char)0x84)
#define KEY_F2          ((unsigned char)0x85)
#define KEY_F3          ((unsigned char)0x86)
#define KEY_F4          ((unsigned char)0x87)

/* Sent to a TTY's input queue when it gains keyboard focus. */
#define KEY_FOCUS_GAIN  ((unsigned char)0x88)

#define KEY_F5          ((unsigned char)0x89)
#define KEY_F6          ((unsigned char)0x8A)
#define KEY_F7          ((unsigned char)0x8B)
#define KEY_F8          ((unsigned char)0x8C)
#define KEY_F9          ((unsigned char)0x8D)
#define KEY_F10         ((unsigned char)0x8E)
#define KEY_F11         ((unsigned char)0x8F)
#define KEY_F12         ((unsigned char)0x90)

/* Modifier-press sentinels.  Emitted on the make event for the modifier
 * itself (release is silent).  Lets diagnostic tools like kbtester light
 * up the cell when the user presses Shift/Ctrl/Alt/Caps alone.  Shells
 * silently drop them (anything < 0x20 except handled sentinels falls
 * through the `c < 0x20 || c > 0x7E` printable filter). */
#define KEY_SHIFT_DOWN  ((unsigned char)0x91)
#define KEY_CTRL_DOWN   ((unsigned char)0x92)
#define KEY_ALT_DOWN    ((unsigned char)0x93)
#define KEY_CAPS_TOGGLE ((unsigned char)0x94)
#define KEY_SUPER_DOWN  ((unsigned char)0x95)
#define KEY_MENU_DOWN   ((unsigned char)0x96)
#define KEY_PAGE_UP     ((unsigned char)0x97)
#define KEY_PAGE_DOWN   ((unsigned char)0x98)
#define KEY_HOME        ((unsigned char)0x99)
#define KEY_END         ((unsigned char)0x9A)
#define KEY_DELETE      ((unsigned char)0x9B)   /* forward-delete (Del key) */

/* Ctrl+C sentinel returned by keyboard_getchar() when a sigint fires. */
#define KEY_CTRL_C      ((unsigned char)0x03)

/* Ctrl+S / Ctrl+Q (editor save / flow-control) -- used by ring-3 apps. */
#define KEY_CTRL_S      ((unsigned char)0x13)
#define KEY_CTRL_Q      ((unsigned char)0x11)

#endif /* _MAKAR_KEYS_H */
