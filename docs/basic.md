---
title: BASIC
nav_order: 8
---

# Makar BASIC (`basic.elf`)

A small C64-flavoured, line-numbered BASIC interpreter that runs as a ring-3
userland program. Source: `src/userspace/basic.c`.

```
basic                 interactive REPL (READY.)
basic prog.bas        load a .bas file, RUN it, then exit
```

**Integer-only by design.** Makar doesn't initialise the x87 FPU or
save/restore it across context switches, so ring-3 floating point would
corrupt under preemption. All arithmetic is 32-bit signed; fractional work
uses fixed-point scaled integers (see the mandelbrot sample).

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

## Statements

`PRINT` (`?`), `LET`/`NAME=expr`, `IF cond THEN <stmt|line>`, `GOTO`,
`GOSUB`/`RETURN`, `FOR..TO..STEP..NEXT`, `INPUT`, `REM`, `END`/`STOP`,
`PAUSE n` (wait n centiseconds, honours Ctrl-C), `CLS`, and graphics
`PLOT`/`LINE`/`RECT`/`COLOR`. Multiple statements per line with `:`.

## Expressions

`+ - * / MOD`, comparisons `= <> < > <= >=` (yield -1/0), `AND`/`OR`, unary `-`,
parentheses. Functions: `ABS()`, `SGN()`, `RND(n)` (0..n-1), and the screen-size
helpers `XMAX` / `YMAX` (last drawable pixel column/row). Variables are 1–2
chars (a letter then optional letter/digit).

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

## Samples

Shipped in `/apps`:

- `mandelbrot.bas` — full-screen Mandelbrot via fixed-point integers (scale
  1000) drawn as a grid of `RECT` blocks across `XMAX`/`YMAX`.
- `lines.bas` — bouncing-endpoint string-art moiré; accumulates until Ctrl-C.

```
basic /apps/mandelbrot.bas
basic /apps/lines.bas
```
