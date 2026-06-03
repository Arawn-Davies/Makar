---
title: BASIC
parent: Using Makar
nav_order: 1
---

# Makar BASIC (`basic.elf`)

A small C64-flavoured, line-numbered BASIC interpreter that runs as a ring-3
userland program. Source: `src/userspace/basic.c`.

```
basic                 interactive REPL (READY.)
basic prog.bas        load a .bas file, RUN it, then exit
```

The interpreter is intentionally integer-only even though the kernel now
initializes and saves/restores x87/SSE state for ring-3 tasks. BASIC keeps a
small, deterministic runtime: all arithmetic is 32-bit signed, and fractional
work uses fixed-point scaled integers. The bundled Mandelbrot sample is the
canonical example of that style.

`basic.elf` is a normal userspace executable. It can be launched directly from
`/apps/sh.elf`, through PATH lookup, or by passing a `.bas` file path.

## REPL

Type a line beginning with a number to store/replace a program line; an empty
body deletes the line. Anything else runs immediately.

| Command | Effect |
|---|---|
| `RUN` | Execute the stored program from the lowest line |
| `LIST` | Print the program |
| `NEW` | Clear the program + variables |
| `BYE` / `EXIT` / `QUIT` | Leave the interpreter |

**Ctrl-C is RUN/STOP:** it breaks a running program back to the `READY.`
prompt (`BREAK IN n`); a second Ctrl-C at the prompt exits to the shell. In
file mode Ctrl-C exits straight to the shell.

When run inside makmux, BASIC draws into the active VT's framebuffer state.
Background VTs keep their own backing grids; a BASIC program on an unfocused VT
does not paint over the currently visible VT.

## Statements

`PRINT` (`?`), `LET`/`NAME=expr`, `IF cond THEN <stmt|line>`, `GOTO`,
`GOSUB`/`RETURN`, `FOR..TO..STEP..NEXT`, `INPUT`, `REM`, `END`/`STOP`,
`PAUSE n` (wait n centiseconds, honours Ctrl-C), `CLS`, and graphics
`PLOT`/`LINE`/`RECT`/`COLOR`. Multiple statements per line with `:`.

Line-numbered storage follows classic BASIC behavior:

- entering `100 PRINT "HELLO"` stores or replaces line 100;
- entering `100` deletes line 100;
- immediate commands such as `PRINT XMAX` run without changing the stored
  program;
- `RUN` starts from the lowest stored line and clears transient loop/gosub
  state.

`IF` accepts either an inline statement or a target line number:

```basic
10 IF X < 10 THEN PRINT X
20 IF X = 10 THEN 100
```

## Expressions

`+ - * / MOD`, comparisons `= <> < > <= >=` (yield -1/0), `AND`/`OR`, unary `-`,
parentheses. Functions: `ABS()`, `SGN()`, `RND(n)` (0..n-1), and the screen-size
helpers `XMAX` / `YMAX` (last drawable pixel column/row). Variables are 1–2
chars (a letter then optional letter/digit).

Comparisons return the traditional BASIC truth values: `-1` for true and `0`
for false. That means expressions such as `(A < B) AND (C < D)` work naturally
with bitwise-style truth values.

## Graphics

Drawing rides the kernel's `SYS_DRAW_LINE` at native VESA resolution (a 1-pixel
line is a point). In VGA-text mode the graphics statements print
`?NO GRAPHICS`.

| Statement | Effect |
|---|---|
| `COLOR c` | Set draw colour (`c` 0–15 = 16-colour palette, or a raw 0xRRGGBB) |
| `PLOT x,y[,c]` | Plot one pixel |
| `LINE x0,y0,x1,y1[,c]` | Draw a line |
| `RECT x0,y0,x1,y1[,c]` | Filled rectangle |

`YMAX` excludes the bottom **makmux** status row, so a program filling
`0..YMAX` fills the screen without clipping the bar. A finished graphics
program waits for a keypress before exiting so the shell's post-exit screen
restore doesn't wipe it instantly.

The graphics statements are intentionally simple wrappers around kernel drawing
syscalls:

- `PLOT` is implemented as a one-pixel line;
- `LINE` maps to `SYS_DRAW_LINE`;
- `RECT` fills by drawing horizontal lines;
- color indices `0..15` use the VGA-style palette, while larger values are
  interpreted as raw RGB.

This keeps BASIC portable across Makar's framebuffer modes without carrying a
separate graphics backend in the interpreter.

## Samples

Shipped in `/apps`:

- `mandelbrot.bas` — full-screen Mandelbrot via fixed-point integers (scale
  1000) drawn as a grid of `RECT` blocks across `XMAX`/`YMAX`.
- `lines.bas` — bouncing-endpoint string-art moiré; accumulates until Ctrl-C.

```
basic /apps/mandelbrot.bas
basic /apps/lines.bas
```
