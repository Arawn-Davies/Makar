# Ctrl+C sigint + calculator app

> **Status:** in progress
> **Branch:** `feat/sigint-and-calc`

## Summary

Ctrl+C terminates any running userspace app and returns to the shell.
`calc` is a bc-style REPL calculator (recursive-descent parser, operator
precedence, parentheses, unary minus, modulo).

## Implemented

- [x] `src/userspace/calc.c` - bc-style expression evaluator
      (grammar: expr / term / unary / factor; no libc dependency)
- [x] `src/userspace/Makefile` - `calc.elf` added to `PROGS`
- [x] `keyboard.h` - arrow-key sentinels moved from 0x01–0x04 to
      0x80–0x83, freeing the Ctrl code range; `KEY_CTRL_C = 0x03` added;
      `keyboard_sigint_consume()` declared
- [x] `keyboard.c` - `g_sigint` flag + `keyboard_sigint_consume()`
      foundation committed

## TODO

- [ ] `keyboard.c` IRQ handler: detect Ctrl+C (`ctrl_pressed && lower_c == 'c'`),
      set `g_sigint = 1`, do NOT push to ring buffer;
      remove old `if (ctrl_code > 4)` guard (arrow keys no longer conflict)
- [ ] `keyboard.c` `keyboard_getchar()`: check `g_sigint` in the wait loop;
      if set, consume and return `KEY_CTRL_C`
- [ ] `shell.c` `shell_readline()`: handle `KEY_CTRL_C` - print `^C\n`,
      call `task_exit()` if in a user task, abandon line if in shell task
- [ ] `shell_cmd_apps.c` `cmd_exec()` wait loop: poll `keyboard_sigint_consume()`;
      if true, mark task DEAD, restore kernel VMM, print `^C\n`, break
