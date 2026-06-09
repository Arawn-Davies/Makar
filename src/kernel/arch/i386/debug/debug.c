/*
 * debug.c - exception handlers and kernel panic screen.
 *
 * Panic rendering:
 *   • VESA path  - when vesa_tty_is_ready(): renders on the active framebuffer,
 *                  no mode switch.  Survives 1080p without touching VGA.
 *   • VGA path   - fallback: writes directly to 0xB8000 (always identity-mapped),
 *                  re-enables VGA display via PAS bit.
 *   Serial output is written first; it is always safe.
 *
 * Layout (both paths) mirrors a Linux/macOS kernel panic:
 *
 *   [title bar]
 *
 *   panic(cpu 0): PAGE FAULT
 *     Faulting address: 0xFD12C000
 *     Error code: 0x00000007  [PROT|WRITE|USER]
 *     at: cmd_panic  (shell_cmds.c:587)
 *
 *   --- Registers -----------------------------------------------
 *   EIP:    0x00401234   EFLAGS: 0x00010282
 *   EAX: 0xDEADBEEF   EBX: 0xCAFEBABE   ECX: 0x00000001   EDX: 0x00000000
 *   ESI: 0x00000000   EDI: 0x00000000   EBP: 0xBFFFEFF8   ESP: 0xBFFFEFF0
 *   CS:  0x00000008   SS:  0x00000010
 *
 *   --- Stack at ESP (0xBFFFEFF0) --------------------------------
 *   +0x00: 0xDEADBEEF   +0x04: 0xCAFEBABE   +0x08: 0x00000000   +0x0C: 0x00000000
 *   +0x10: 0x00000000   +0x14: 0x00000000   +0x18: 0x00000000   +0x1C: 0x00000000
 *
 *   ------------------------------------------------------------
 *   Kernel panic - not syncing: PAGE FAULT
 *   System halted. Please restart manually.
 */

#include <kernel/debug.h>
#include <kernel/isr.h>
#include <kernel/serial.h>
#include <kernel/asm.h>
#include <kernel/tty.h>
#include <kernel/vesa.h>
#include <kernel/vesa_tty.h>
#include <kernel/vesa_font.h>
#include <kernel/video.h>
#include <kernel/bochs_vbe.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/paging.h>
#include <kernel/task.h>
#include <kernel/signal.h>
#include <stddef.h>
#include <string.h>

/* ============================================================
 * Pure formatting helpers - no I/O, no external dependencies
 * ============================================================ */

static const char HEX[] = "0123456789ABCDEF";

static void fmt_hex32(char buf[11], uint32_t v)
{
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++)
        buf[2 + i] = HEX[(v >> ((7 - i) * 4)) & 0xF];
    buf[10] = '\0';
}

static void fmt_dec(char buf[12], uint32_t v)
{
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[12]; int n = 0;
    for (; v; v /= 10) tmp[n++] = '0' + (v % 10);
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
}

static int slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

/* Return the filename component of a path (after last '/'). */
static const char *basename_s(const char *path)
{
    if (!path) return NULL;
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') last = p + 1;
    return last;
}

/* ============================================================
 * Serial helpers
 * ============================================================ */

static void ser_hex(uint32_t v) { char b[11]; fmt_hex32(b, v); Serial_WriteString(b); }

static void serial_dump(registers_t *r)
{
    Serial_WriteString("EIP=");    ser_hex(r->eip);
    Serial_WriteString("  EFLAGS="); ser_hex(r->eflags); Serial_WriteChar('\n');
    Serial_WriteString("EAX=");    ser_hex(r->eax);
    Serial_WriteString("  EBX=");  ser_hex(r->ebx);
    Serial_WriteString("  ECX=");  ser_hex(r->ecx);
    Serial_WriteString("  EDX=");  ser_hex(r->edx); Serial_WriteChar('\n');
    Serial_WriteString("ESI=");    ser_hex(r->esi);
    Serial_WriteString("  EDI=");  ser_hex(r->edi);
    Serial_WriteString("  EBP=");  ser_hex(r->ebp);
    Serial_WriteString("  ESP=");  ser_hex(r->esp); Serial_WriteChar('\n');
    Serial_WriteString("CS=");     ser_hex(r->cs);
    Serial_WriteString("  SS=");   ser_hex(r->ss);  Serial_WriteChar('\n');
    /* Stack dump */
    if (r->esp >= 0x100000u && r->esp < 0x10000000u) {
        Serial_WriteString("Stack at ESP:\n");
        const uint32_t *sp = (const uint32_t *)r->esp;
        for (int i = 0; i < 8; i++) {
            Serial_WriteString("  +0x");
            Serial_WriteChar(HEX[(i*4 >> 4) & 0xF]);
            Serial_WriteChar(HEX[(i*4)       & 0xF]);
            Serial_WriteString(": ");
            ser_hex(sp[i]);
            Serial_WriteChar('\n');
        }
    }
}

/* ============================================================
 * VGA text panic renderer  (80 × 25, 0xB8000)
 * ============================================================ */

#define VGA_COLS  80
#define VGA_ROWS  25
#define VGA_BASE  ((volatile uint16_t *)0xB8000)

/* VGA attribute bytes */
#define A_TITLE  0x4Fu   /* bright white on red          */
#define A_BODY   0x0Fu   /* bright white on black        */
#define A_DIM    0x08u   /* dark grey on black           */
#define A_SECT   0x07u   /* light grey on black          */
#define A_VAL    0x0Eu   /* yellow on black              */
#define A_FOOT   0x07u   /* light grey on black (footer) */

static void vga_putc(int col, int row, char c, uint8_t a)
{
    int rows = (int)(t_height > 0 ? t_height : (size_t)VGA_ROWS);
    if (col >= 0 && col < VGA_COLS && row >= 0 && row < rows)
        VGA_BASE[row * VGA_COLS + col] = (uint16_t)((a << 8) | (uint8_t)c);
}

static int vga_puts(int col, int row, const char *s, uint8_t a)
{
    while (s && *s && col < VGA_COLS) vga_putc(col++, row, *s++, a);
    return col;
}

static int vga_hex(int col, int row, uint32_t v, uint8_t a)
{
    char buf[11]; fmt_hex32(buf, v);
    return vga_puts(col, row, buf, a);
}

static void vga_fill_row(int row, uint8_t a)
{
    for (int x = 0; x < VGA_COLS; x++) vga_putc(x, row, ' ', a);
}

static void vga_center(int row, const char *s, uint8_t a)
{
    vga_fill_row(row, a);
    int col = (VGA_COLS - slen(s)) / 2;
    vga_puts(col > 0 ? col : 0, row, s, a);
}

/* Fill from col to end of row with character c. */
static void vga_fill_to_eol(int col, int row, char c, uint8_t a)
{
    for (; col < VGA_COLS; col++) vga_putc(col, row, c, a);
}

/* Force the display all the way back to a legacy VGA text mode (mode 3, 80x25)
 * so the 0xB8000 text buffer is scanned out and writable.  This is the
 * older-Windows / Linux-vgacon approach for a panic: never trust the
 * accelerated driver, the command FIFO, or the (possibly mid-reconfiguration
 * or unmapped) high-res framebuffer -- a graphics panic renderer that faults
 * there cascades the fault and hides the panic (the 1080p "CPU disabled" with
 * no message).  Touches only I/O ports and 0xB8000, both always available on
 * this BIOS/VGA-compatible target, so it cannot itself fault.
 *
 *   1. video_svga2_to_vga() -- SVGA II off (no-op if not bound)
 *   2. bochs_vbe_disable()  -- Bochs DISPI off (harmless if absent) + program
 *                              IBM VGA mode 3 registers + reload the 8x16 font
 *   3. unblank the attribute controller
 *   4. pin the text grid to 80x25 so the text renderer lays out correctly
 *      (t_height may still hold the old VESA/80x50 row count). */
static void force_vga_text_mode(void)
{
    video_svga2_to_vga();
    bochs_vbe_disable();
    inb(0x3DA); outb(0x3C0, 0x20);
    t_height = VGA_ROWS;
}

/* Last-ditch, fault-proof panic notice.  Used only when the normal panic
 * renderer itself faulted (e.g. it wrote into a framebuffer a mid-switch
 * setmode left unmapped).  Touches nothing but I/O ports and the VGA text
 * buffer at 0xB8000, which lives in the first 4 MiB large page and is
 * therefore present in every page directory -- it cannot fault.  Forces the
 * CRTC back to text scanout so the message is visible even if the hardware
 * was in a VESA graphics mode. */
static void panic_textmode_notice(void)
{
    force_vga_text_mode();   /* SVGA II off + VGA mode 3, not just DISPI off */
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA_BASE[i] = (uint16_t)(0x4F00u | (uint8_t)' ');   /* white on red */
    vga_center(VGA_ROWS / 2 - 1, "*** MAKAR KERNEL PANIC ***", 0x4Fu);
    vga_center(VGA_ROWS / 2 + 1,
               "Press any key to reboot. Details on serial.",
               0x4Fu);
}

static void render_panic_vga(const char *fault_type, const char *msg,
                              const char *file, const char *func, int line,
                              uint32_t fault_addr, int show_addr,
                              registers_t *r)
{
    int rows = (int)(t_height > 0 ? t_height : (size_t)VGA_ROWS);
    for (int y = 0; y < rows; y++) vga_fill_row(y, A_BODY);

    /* ---- Title bar ---- */
    vga_fill_row(0, A_TITLE);
    vga_center(0, "*** MAKAR KERNEL PANIC ***", A_TITLE);

    int row = 2, col;

    /* ---- panic(...): line ---- */
    col = vga_puts(1, row, "panic(cpu 0): ", A_DIM);
    if (fault_type)      vga_puts(col, row, fault_type, A_BODY);
    else if (msg)        vga_puts(col, row, msg,        A_BODY);
    row++;

    if (show_addr) {
        col = vga_puts(4, row, "Faulting address: ", A_DIM);
        vga_hex(col, row, fault_addr, A_VAL); row++;
    }
    if (r && show_addr) {
        col = vga_puts(4, row, "Error code: ", A_DIM);
        col = vga_hex(col, row, r->err_code, A_VAL);
        col = vga_puts(col, row, "  [", A_DIM);
        col = vga_puts(col, row, (r->err_code & 1) ? "PROT"   : "NP",      A_BODY);
        col = vga_puts(col, row, (r->err_code & 2) ? "|WRITE" : "|READ",   A_BODY);
        col = vga_puts(col, row, (r->err_code & 4) ? "|USER"  : "|KERNEL", A_BODY);
        vga_puts(col, row, "]", A_DIM); row++;
    }
    if (fault_type && msg) {
        /* Exception + separate message (rare) */
        col = vga_puts(4, row, "Message: ", A_DIM);
        vga_puts(col, row, msg, A_BODY); row++;
    }
    if (func) {
        col = vga_puts(4, row, "at: ", A_DIM);
        col = vga_puts(col, row, func, A_VAL);
        const char *fn = basename_s(file);
        if (fn) {
            col = vga_puts(col, row, "  (", A_DIM);
            col = vga_puts(col, row, fn,    A_SECT);
            col = vga_puts(col, row, ":",   A_DIM);
            char lbuf[12]; fmt_dec(lbuf, (uint32_t)line);
            col = vga_puts(col, row, lbuf,  A_VAL);
            vga_puts(col, row, ")", A_DIM);
        } else if (line > 0) {
            col = vga_puts(col, row, "  line ", A_DIM);
            char lbuf[12]; fmt_dec(lbuf, (uint32_t)line);
            vga_puts(col, row, lbuf, A_VAL);
        }
        row++;
    }

    /* ---- Faulting context: which program / driver, and how to resolve EIP --- */
    {
        task_t *cur = task_current();
        int ring3 = (r && (r->cs & 3) == 3);
        col = vga_puts(4, row, "Context: ", A_DIM);
        if (ring3) {
            col = vga_puts(col, row, "program '", A_DIM);
            col = vga_puts(col, row, (cur && cur->name) ? cur->name : "?", A_VAL);
            vga_puts(col, row, "' (ring 3 userspace)", A_DIM);
        } else {
            col = vga_puts(col, row, "kernel (ring 0)", A_VAL);
            if (cur && cur->name) {
                col = vga_puts(col, row, ", in task '", A_DIM);
                col = vga_puts(col, row, cur->name, A_VAL);
                vga_puts(col, row, "'", A_DIM);
            }
        }
        row++;
        /* Offline symbolisation: addr2line maps EIP -> exact function + .c:line.
         * Userspace ELFs link at 0x40000000 and the kernel image carries its own
         * symbols, so the raw EIP works against either with no adjustment. */
        if (r) {
            col = vga_puts(4, row, "Resolve: addr2line -e ", A_DIM);
            col = vga_puts(col, row, ring3 ? "<program>.elf " : "makar.kernel ", A_SECT);
            vga_hex(col, row, r->eip, A_VAL);
            row++;
        }
    }

    row++; /* blank */

    /* ---- Registers ---- */
    if (r) {
        col = vga_puts(1, row, "--- Registers ", A_SECT);
        vga_fill_to_eol(col, row, '-', A_DIM); row++;

        /* EIP + EFLAGS */
        col = vga_puts(1, row, "EIP:    ", A_DIM);
        col = vga_hex(col, row, r->eip,    A_VAL);
        col = vga_puts(col, row, "   EFLAGS: ", A_DIM);
              vga_hex(col, row, r->eflags, A_VAL); row++;

        /* EAX EBX ECX EDX */
        col = vga_puts(1, row, "EAX: ", A_DIM); col = vga_hex(col, row, r->eax, A_VAL);
        col = vga_puts(col, row, "   EBX: ", A_DIM); col = vga_hex(col, row, r->ebx, A_VAL);
        col = vga_puts(col, row, "   ECX: ", A_DIM); col = vga_hex(col, row, r->ecx, A_VAL);
        col = vga_puts(col, row, "   EDX: ", A_DIM);       vga_hex(col, row, r->edx, A_VAL);
        row++;

        /* ESI EDI EBP ESP */
        col = vga_puts(1, row, "ESI: ", A_DIM); col = vga_hex(col, row, r->esi, A_VAL);
        col = vga_puts(col, row, "   EDI: ", A_DIM); col = vga_hex(col, row, r->edi, A_VAL);
        col = vga_puts(col, row, "   EBP: ", A_DIM); col = vga_hex(col, row, r->ebp, A_VAL);
        col = vga_puts(col, row, "   ESP: ", A_DIM);       vga_hex(col, row, r->esp, A_VAL);
        row++;

        /* CS SS */
        col = vga_puts(1, row, "CS:  ", A_DIM); col = vga_hex(col, row, r->cs, A_VAL);
        col = vga_puts(col, row, "   SS:  ", A_DIM);       vga_hex(col, row, r->ss, A_VAL);
        row++;

        row++; /* blank */

        /* ---- Stack dump ---- */
        if (r->esp >= 0x100000u && r->esp < 0x10000000u && row < rows - 6) {
            col = vga_puts(1, row, "--- Stack at ESP (", A_SECT);
            col = vga_hex(col, row, r->esp, A_VAL);
            col = vga_puts(col, row, ") ", A_DIM);
            vga_fill_to_eol(col, row, '-', A_DIM); row++;

            const uint32_t *sp = (const uint32_t *)r->esp;
            /* 4 entries per row: +0x00..+0x1C */
            for (int i = 0; i < 8 && row < rows - 3; i += 4) {
                col = 1;
                for (int j = i; j < i + 4 && j < 8; j++) {
                    col = vga_puts(col, row, "+0x", A_DIM);
                    vga_putc(col, row, HEX[(j*4 >> 4) & 0xF], A_DIM); col++;
                    vga_putc(col, row, HEX[(j*4)       & 0xF], A_DIM); col++;
                    col = vga_puts(col, row, ": ", A_DIM);
                    col = vga_hex(col, row, sp[j], A_VAL);
                    col = vga_puts(col, row, "   ", A_DIM);
                }
                row++;
            }
        }
    }

    /* ---- Footer ---- */
    for (int x = 0; x < VGA_COLS; x++) vga_putc(x, rows - 3, '-', A_DIM);

    col = vga_puts(1, rows - 2, "Kernel panic - not syncing: ", A_FOOT);
    if (fault_type) col = vga_puts(col, rows - 2, fault_type, A_BODY);
    if (msg)              vga_puts(col, rows - 2, msg,        A_BODY);

    vga_puts(1, rows - 1, "Halted (CPU alive). Press a key to reboot, or reset the VM.", A_DIM);
}

/* ============================================================
 * VESA framebuffer panic renderer
 * ============================================================ */

static uint32_t pv_rgb(const vesa_fb_t *fb, uint32_t rgb)
{
    return ((uint32_t)((rgb >> 16) & 0xFF) << fb->red_shift)   |
           ((uint32_t)((rgb >>  8) & 0xFF) << fb->green_shift) |
           ((uint32_t)((rgb      ) & 0xFF) << fb->blue_shift);
}

static void pv_pixel(const vesa_fb_t *fb, uint32_t x, uint32_t y, uint32_t c)
{
    if (x >= fb->width || y >= fb->height) return;
    uint32_t bpp = fb->bpp / 8;
    uint8_t *p = (uint8_t *)fb->addr + y * fb->pitch + x * bpp;
    for (uint32_t i = 0; i < bpp && i < 4; i++) p[i] = (uint8_t)(c >> (i * 8));
}

static void pv_fill_rect(const vesa_fb_t *fb,
                          uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                          uint32_t c)
{
    for (uint32_t y = y0; y < y0 + h && y < fb->height; y++)
        for (uint32_t x = x0; x < x0 + w && x < fb->width; x++)
            pv_pixel(fb, x, y, c);
}

/* Draw a 1-pixel-tall horizontal line across the full screen width. */
static void pv_hline(const vesa_fb_t *fb, uint32_t py, uint32_t c)
{
    for (uint32_t x = 0; x < fb->width; x++)
        pv_pixel(fb, x, py, c);
}

/* Render one character glyph at pixel (px, py). Returns next px. */
static uint32_t pv_char(const vesa_fb_t *fb,
                         uint32_t px, uint32_t py, char c,
                         uint32_t fg, uint32_t bg, uint32_t scale)
{
    uint8_t idx = (uint8_t)c;
    if (idx >= 128) idx = '?';
    const uint8_t *glyph = FONT8x8[idx];
    uint32_t cw = 8 * scale, ch = 8 * scale;
    pv_fill_rect(fb, px, py, cw, ch, bg);
    for (uint32_t gy = 0; gy < 8; gy++)
        for (uint32_t gx = 0; gx < 8; gx++)
            if (glyph[gy] & (1u << gx))
                for (uint32_t sy = 0; sy < scale; sy++)
                    for (uint32_t sx = 0; sx < scale; sx++)
                        pv_pixel(fb, px + gx*scale + sx, py + gy*scale + sy, fg);
    return px + cw;
}

static uint32_t pv_str(const vesa_fb_t *fb,
                        uint32_t px, uint32_t py,
                        const char *s, uint32_t fg, uint32_t bg, uint32_t scale)
{
    while (s && *s) px = pv_char(fb, px, py, *s++, fg, bg, scale);
    return px;
}

static uint32_t pv_hex(const vesa_fb_t *fb,
                        uint32_t px, uint32_t py, uint32_t v,
                        uint32_t fg, uint32_t bg, uint32_t scale)
{
    char buf[11]; fmt_hex32(buf, v);
    return pv_str(fb, px, py, buf, fg, bg, scale);
}

static uint32_t pv_dec(const vesa_fb_t *fb,
                        uint32_t px, uint32_t py, uint32_t v,
                        uint32_t fg, uint32_t bg, uint32_t scale)
{
    char buf[12]; fmt_dec(buf, v);
    return pv_str(fb, px, py, buf, fg, bg, scale);
}

/*
 * Draw a section header:  "--- <title> ---...---"  (dashes fill to edge)
 */
static uint32_t pv_section(const vesa_fb_t *fb,
                             uint32_t px, uint32_t py,
                             const char *title,
                             uint32_t fg_dash, uint32_t fg_title,
                             uint32_t bg, uint32_t scale)
{
    uint32_t cw = 8 * scale;
    px = pv_str(fb, px, py, "--- ", fg_dash, bg, scale);
    px = pv_str(fb, px, py, title, fg_title, bg, scale);
    px = pv_str(fb, px, py, " ", fg_dash, bg, scale);
    /* fill rest of row with dashes */
    while (px + cw <= fb->width) {
        px = pv_char(fb, px, py, '-', fg_dash, bg, scale);
    }
    return px;
}

/* Framebuffer (graphics) panic renderer.  No longer on the panic path -- panics
 * now drop to legacy VGA text (force_vga_text_mode + render_panic_vga) because
 * the framebuffer can be the very thing that failed.  Retained for a future
 * UEFI/GOP target that has no VGA text mode to fall back to. */
__attribute__((unused))
static void render_panic_vesa(const vesa_fb_t *fb,
                               const char *fault_type, const char *msg,
                               const char *file, const char *func, int line,
                               uint32_t fault_addr, int show_addr,
                               registers_t *r)
{
    uint32_t scale = (fb->width >= 1280) ? 2u : 1u;
    uint32_t cw    = 8u * scale;
    uint32_t ch    = 8u * scale;
    uint32_t rows  = fb->height / ch;

    /* Colour palette (native pixel format) */
    uint32_t C_BG      = pv_rgb(fb, 0x0D0D1A);  /* very dark navy         */
    uint32_t C_TITLE_BG= pv_rgb(fb, 0xA00000);  /* deep red bar           */
    uint32_t C_WHITE   = pv_rgb(fb, 0xEEEEEE);  /* body text              */
    uint32_t C_GREY    = pv_rgb(fb, 0x8899AA);  /* labels / dim text      */
    uint32_t C_YELL    = pv_rgb(fb, 0xFFCC44);  /* register values        */
    uint32_t C_RED     = pv_rgb(fb, 0xFF6644);  /* fault / error values   */
    uint32_t C_CYAN    = pv_rgb(fb, 0x66CCFF);  /* section header text    */
    uint32_t C_SEP     = pv_rgb(fb, 0x334455);  /* separator line colour  */
    uint32_t C_FOOT_BG = pv_rgb(fb, 0x1A0000);  /* footer row background  */

    /* Clear screen */
    pv_fill_rect(fb, 0, 0, fb->width, fb->height, C_BG);

    /* ---- Title bar ---- */
    uint32_t bar_h = ch + ch / 2;
    pv_fill_rect(fb, 0, 0, fb->width, bar_h, C_TITLE_BG);
    /* "Makar" left, "Kernel Panic" right, kernel version centred */
    pv_str(fb, cw, (bar_h - ch) / 2, "Makar",         C_WHITE, C_TITLE_BG, scale);
    const char *kp = "Kernel Panic";
    uint32_t kp_px = fb->width - (uint32_t)slen(kp) * cw - cw;
    pv_str(fb, kp_px, (bar_h - ch) / 2, kp,            C_WHITE, C_TITLE_BG, scale);
    /* separator line below title */
    pv_hline(fb, bar_h, C_SEP);

    uint32_t row = bar_h / ch + 2;  /* current character row */
#define LM  (2u * cw)              /* left margin in pixels  */
#define PY  (row * ch)
#define NR  do { row++; } while(0)

    /* ---- panic(cpu 0): line ---- */
    {
        uint32_t px = pv_str(fb, LM, PY, "panic(cpu 0): ", C_GREY, C_BG, scale);
        if (fault_type) px = pv_str(fb, px, PY, fault_type, C_RED,   C_BG, scale);
        if (msg && !fault_type) pv_str(fb, px, PY, msg,     C_WHITE, C_BG, scale);
        NR;
    }

    /* ---- Faulting address ---- */
    if (show_addr) {
        uint32_t px = pv_str(fb, LM + 2*cw, PY, "Faulting address:  ", C_GREY, C_BG, scale);
        pv_hex(fb, px, PY, fault_addr, C_RED, C_BG, scale); NR;
    }

    /* ---- Error code + bits (page fault) ---- */
    if (r && show_addr) {
        uint32_t px = pv_str(fb, LM + 2*cw, PY, "Error code:  ", C_GREY, C_BG, scale);
        px = pv_hex(fb, px, PY, r->err_code, C_YELL, C_BG, scale);
        px = pv_str(fb, px, PY, "  [", C_GREY, C_BG, scale);
        px = pv_str(fb, px, PY, (r->err_code & 1) ? "PROT"   : "NP",      C_WHITE, C_BG, scale);
        px = pv_str(fb, px, PY, (r->err_code & 2) ? "|WRITE" : "|READ",   C_WHITE, C_BG, scale);
        px = pv_str(fb, px, PY, (r->err_code & 4) ? "|USER"  : "|KERNEL", C_WHITE, C_BG, scale);
        pv_str(fb, px, PY, "]", C_GREY, C_BG, scale); NR;
    }

    /* ---- Message (when both fault_type and msg are set) ---- */
    if (fault_type && msg) {
        uint32_t px = pv_str(fb, LM + 2*cw, PY, "Message:  ", C_GREY, C_BG, scale);
        pv_str(fb, px, PY, msg, C_WHITE, C_BG, scale); NR;
    }

    /* ---- Source location ---- */
    if (func) {
        uint32_t px = pv_str(fb, LM + 2*cw, PY, "at: ", C_GREY, C_BG, scale);
        px = pv_str(fb, px, PY, func, C_YELL, C_BG, scale);
        const char *fn = basename_s(file);
        if (fn) {
            px = pv_str(fb, px, PY, "  (", C_GREY, C_BG, scale);
            px = pv_str(fb, px, PY, fn,    C_CYAN, C_BG, scale);
            px = pv_str(fb, px, PY, ":",   C_GREY, C_BG, scale);
            px = pv_dec(fb, px, PY, (uint32_t)line, C_YELL, C_BG, scale);
            pv_str(fb, px, PY, ")", C_GREY, C_BG, scale);
        } else if (line > 0) {
            px = pv_str(fb, px, PY, "  line ", C_GREY, C_BG, scale);
            pv_dec(fb, px, PY, (uint32_t)line, C_YELL, C_BG, scale);
        }
        NR;
    }

    NR; /* blank */

    /* ---- Registers section ---- */
    if (r) {
        pv_section(fb, LM, PY, "Registers", C_SEP, C_CYAN, C_BG, scale); NR;

        /* EIP + EFLAGS */
        {
            uint32_t px = pv_str(fb, LM, PY, "EIP:    ", C_GREY, C_BG, scale);
            px = pv_hex(fb, px, PY, r->eip,    C_YELL, C_BG, scale);
            px = pv_str(fb, px, PY, "   EFLAGS: ", C_GREY, C_BG, scale);
            pv_hex(fb, px, PY, r->eflags, C_YELL, C_BG, scale); NR;
        }

        /* General-purpose registers: 4 per row */
        typedef struct { const char *n; uint32_t v; } reg_t;
        reg_t gp[] = {
            {"EAX: ", r->eax}, {"EBX: ", r->ebx},
            {"ECX: ", r->ecx}, {"EDX: ", r->edx},
            {"ESI: ", r->esi}, {"EDI: ", r->edi},
            {"EBP: ", r->ebp}, {"ESP: ", r->esp},
        };
        for (int i = 0; i < 8; i++) {
            if (i % 4 == 0) {
                if (i) NR;
                pv_fill_rect(fb, 0, PY, fb->width, ch, C_BG);
            }
            uint32_t px = LM + (uint32_t)(i % 4) * cw * 19u;
            uint32_t py = row * ch;
            px = pv_str(fb, px, py, gp[i].n, C_GREY, C_BG, scale);
            pv_hex(fb, px, py, gp[i].v, C_YELL, C_BG, scale);
        }
        NR;

        /* CS + SS */
        {
            uint32_t px = pv_str(fb, LM, PY, "CS:  ", C_GREY, C_BG, scale);
            px = pv_hex(fb, px, PY, r->cs, C_YELL, C_BG, scale);
            px = pv_str(fb, px, PY, "   SS:  ", C_GREY, C_BG, scale);
            pv_hex(fb, px, PY, r->ss, C_YELL, C_BG, scale); NR;
        }

        NR; /* blank */

        /* ---- Stack dump ---- */
        if (r->esp >= 0x100000u && r->esp < 0x10000000u && row + 4 < rows) {
            /* Section header with ESP address embedded */
            {
                uint32_t px = pv_str(fb, LM, PY, "--- Stack at ESP (", C_SEP, C_BG, scale);
                px = pv_hex(fb, px, PY, r->esp, C_YELL, C_BG, scale);
                px = pv_str(fb, px, PY, ") ", C_SEP, C_BG, scale);
                while (px + cw <= fb->width)
                    px = pv_char(fb, px, PY, '-', C_SEP, C_BG, scale);
                NR;
            }

            const uint32_t *sp = (const uint32_t *)r->esp;
            for (int i = 0; i < 8; i++) {
                if (i % 4 == 0) {
                    if (i) NR;
                    pv_fill_rect(fb, 0, PY, fb->width, ch, C_BG);
                }
                uint32_t px = LM + (uint32_t)(i % 4) * cw * 19u;
                uint32_t py = row * ch;
                px = pv_str(fb, px, py, "+0x", C_GREY, C_BG, scale);
                px = pv_char(fb, px, py, HEX[(i*4 >> 4) & 0xF], C_GREY, C_BG, scale);
                px = pv_char(fb, px, py, HEX[(i*4)       & 0xF], C_GREY, C_BG, scale);
                px = pv_str(fb, px, py, ": ", C_GREY, C_BG, scale);
                pv_hex(fb, px, py, sp[i], C_YELL, C_BG, scale);
            }
            NR; NR;
        }
    }

    /* ---- Footer ---- */
    if (rows >= 4) {
        uint32_t fy = (rows - 3) * ch;
        pv_hline(fb, fy - 2, C_SEP);
        pv_fill_rect(fb, 0, fy, fb->width, 3 * ch, C_FOOT_BG);

        uint32_t px = pv_str(fb, LM, fy + ch / 4,
                             "Kernel panic - not syncing: ", C_GREY, C_FOOT_BG, scale);
        if (fault_type) px = pv_str(fb, px, fy + ch/4, fault_type, C_RED,   C_FOOT_BG, scale);
        if (msg)              pv_str(fb, px, fy + ch/4, msg,        C_WHITE, C_FOOT_BG, scale);

        pv_str(fb, LM, fy + ch + ch/4,
               "System halted. Please restart manually.",
               C_GREY, C_FOOT_BG, scale);
    }

#undef LM
#undef PY
#undef NR
}

/* ============================================================
 * Main panic dispatcher
 * ============================================================ */

/* Terminal state for a panic.  We deliberately do NOT `cli; hlt`: a CPU halted
 * with interrupts masked is exactly what VMware / VirtualBox report as "the CPU
 * has been disabled by the guest operating system" -- which pops a host dialog
 * and captures the host mouse pointer, hiding the panic we just rendered.
 * Instead keep the VCPU executing (so the panic stays on screen and the host
 * pointer stays free) and poll the 8042 for a keystroke; any key reboots via
 * the keyboard-controller CPU-reset pulse, so the user escapes cleanly without
 * resetting the VM by hand.  Interrupts stay masked -- we poll, never take an
 * IRQ -- so no handler can run and re-fault after the panic. */
static __attribute__((noreturn)) void panic_halt(void)
{
    asm volatile("cli");

    /* Drain bytes already sitting in the 8042 -- notably the *release* codes of
     * the panic chord itself -- so we don't immediately "see a key" and reboot
     * before the user can read the panic. */
    for (int t = 0; t < 100000 && (inb(0x64) & 0x01); t++)
        (void)inb(0x60);

    /* Spin alive (never cli;hlt -- that makes the VM report the CPU "disabled"
     * and grab the host mouse) until a fresh key *press*, then reboot. */
    for (;;) {
        if (inb(0x64) & 0x01) {              /* a fresh scancode arrived */
            uint8_t sc = inb(0x60);
            /* Only a make code (key DOWN) reboots.  Ignore break codes (bit 7)
             * and the e0/e1 extended prefixes -- otherwise merely *releasing*
             * the Ctrl+Alt+Shift+P panic chord would reboot before the user can
             * read the panic. */
            if (sc == 0xE0 || sc == 0xE1 || (sc & 0x80))
                continue;
            /* Reboot via the 8042: wait for its input buffer to drain, then
             * pulse the CPU reset line. */
            for (int t = 0; t < 100000 && (inb(0x64) & 0x02); t++)
                ;
            outb(0x64, 0xFE);
            /* If the controller didn't take it, force a triple fault: load a
             * zero-limit IDT and raise an interrupt the CPU can't deliver. */
            for (int t = 0; t < 2000000; t++)
                asm volatile("pause");
            struct { uint16_t limit; uint32_t base; } __attribute__((packed))
                null_idt = { 0, 0 };
            asm volatile("lidt %0; int3" :: "m"(null_idt));
        }
        asm volatile("pause");
    }
}

/* Set once we begin painting a panic.  If the renderer itself faults -- e.g. it
 * writes into a framebuffer region that a mid-switch setmode left unmapped --
 * the page-fault handler re-enters kernel_panic; without this guard that
 * recurses forever (a CPU exception ignores the `cli` below), which is exactly
 * the panic loop seen switching to 1080p.  On re-entry we skip the renderer
 * (serial already has the first, complete report) and just stop the CPU. */
static volatile int s_in_panic = 0;

static void kernel_panic(const char *fault_type, const char *msg,
                          const char *file, const char *func, int line,
                          uint32_t fault_addr, int show_addr,
                          registers_t *r)
{
    if (s_in_panic) {
        s_in_panic++;
        Serial_WriteString("panic: nested fault while rendering panic; halting\n");
        /* First nesting only: try the fault-proof text-mode notice so the
         * user still sees a reboot instruction.  If even that faults
         * (s_in_panic > 2, which shouldn't be possible), skip straight to
         * the halt -- never recurse, never loop. */
        if (s_in_panic == 2)
            panic_textmode_notice();
        panic_halt();
    }
    s_in_panic = 1;

    /* 1. Serial - always written first; always safe */
    Serial_WriteString("\n\n");
    Serial_WriteString("panic(cpu 0): ");
    if (fault_type) Serial_WriteString((char *)fault_type);
    if (msg && !fault_type) Serial_WriteString((char *)msg);
    Serial_WriteChar('\n');
    if (show_addr) {
        Serial_WriteString("  Faulting address:  "); ser_hex(fault_addr); Serial_WriteChar('\n');
    }
    if (r && show_addr) {
        Serial_WriteString("  Error code:  "); ser_hex(r->err_code);
        Serial_WriteString("  [");
        Serial_WriteString((r->err_code & 1) ? "PROT"   : "NP");
        Serial_WriteString((r->err_code & 2) ? "|WRITE" : "|READ");
        Serial_WriteString((r->err_code & 4) ? "|USER"  : "|KERNEL");
        Serial_WriteString("]\n");
    }
    if (fault_type && msg) {
        Serial_WriteString("  Message:  "); Serial_WriteString((char *)msg); Serial_WriteChar('\n');
    }
    if (func) {
        char lbuf[12]; fmt_dec(lbuf, (uint32_t)line);
        Serial_WriteString("  at: ");
        Serial_WriteString((char *)func);
        if (file) {
            Serial_WriteString("  (");
            Serial_WriteString((char *)basename_s(file));
            Serial_WriteString(":"); Serial_WriteString(lbuf);
            Serial_WriteString(")");
        }
        Serial_WriteChar('\n');
    }
    /* Which task was running when this fired?  Useful even for ring-0
     * panics (e.g. a kernel-side null-deref while servicing a syscall
     * tells you which userspace process triggered the path). */
    task_t *cur = task_current();
    if (cur) {
        Serial_WriteString("  Task: pid=");
        Serial_WriteDec((uint32_t)cur->pid);
        Serial_WriteString(" (");
        Serial_WriteString((char *)(cur->name ? cur->name : "?"));
        Serial_WriteString(")");
        if (r) {
            Serial_WriteString(" ring=");
            Serial_WriteString((r->cs & 3) == 3 ? "3" : "0");
        }
        Serial_WriteChar('\n');
    }
    /* Offline symbolisation hint: addr2line maps EIP -> function + .c:line.
     * Userspace ELFs link at 0x40000000 and the kernel image carries symbols,
     * so the raw EIP resolves against either with no adjustment. */
    if (r) {
        int ring3 = (r->cs & 3) == 3;
        Serial_WriteString("  Resolve: addr2line -e ");
        Serial_WriteString(ring3 ? "<program>.elf " : "makar.kernel ");
        ser_hex(r->eip);
        Serial_WriteChar('\n');
    }

    Serial_WriteString("--- registers ---\n");
    if (r) serial_dump(r);

    /* 2. Drop to legacy VGA text mode and render the panic there.  We do NOT
     * paint the panic into the high-res framebuffer: at panic time the
     * accelerated driver / FIFO / framebuffer may be exactly what failed (or be
     * mid-reconfiguration / partially mapped), and a graphics renderer that
     * faults there cascades into a triple fault that disables the CPU with
     * nothing shown.  VGA mode 3 + the 0xB8000 text buffer always work on this
     * target and the renderer cannot fault. */
    force_vga_text_mode();
    render_panic_vga(fault_type, msg, file, func, line,
                     fault_addr, show_addr, r);

    /* 3. Spin alive (NOT cli;hlt) so the VM doesn't flag the CPU "disabled";
     * any key reboots.  See panic_halt(). */
    panic_halt();
}

/* ============================================================
 * CPU state capture for software panics (no hardware exception frame)
 *
 * __attribute__((noinline)) so __builtin_return_address(0) gives the
 * address inside kpanic/kpanic_at that called us - i.e. the panic site.
 * ============================================================ */

static void __attribute__((noinline)) capture_cpu_state(registers_t *r)
{
    /*
     * Pass r as a single register input; address each field with a compile-time
     * constant offset via %c[name].  This avoids the "impossible constraints"
     * error that arises when 8+ "=m" outputs share one asm block on i386.
     */
    asm volatile(
        "movl %%eax, %c[eax](%0)\n\t"
        "movl %%ebx, %c[ebx](%0)\n\t"
        "movl %%ecx, %c[ecx](%0)\n\t"
        "movl %%edx, %c[edx](%0)\n\t"
        "movl %%esi, %c[esi](%0)\n\t"
        "movl %%edi, %c[edi](%0)\n\t"
        "movl %%ebp, %c[ebp](%0)\n\t"
        "movl %%esp, %c[esp](%0)\n\t"
        "pushfl\n\t"
        "popl  %c[efl](%0)\n\t"
        :
        : "r"(r),
          [eax]"i"(offsetof(registers_t, eax)),
          [ebx]"i"(offsetof(registers_t, ebx)),
          [ecx]"i"(offsetof(registers_t, ecx)),
          [edx]"i"(offsetof(registers_t, edx)),
          [esi]"i"(offsetof(registers_t, esi)),
          [edi]"i"(offsetof(registers_t, edi)),
          [ebp]"i"(offsetof(registers_t, ebp)),
          [esp]"i"(offsetof(registers_t, esp)),
          [efl]"i"(offsetof(registers_t, eflags))
        : "memory"
    );

    /* Segment registers - 16-bit source, zero-extend to 32-bit field */
    { uint16_t v; asm volatile("movw %%cs, %0" : "=r"(v)); r->cs = v; }
    { uint16_t v; asm volatile("movw %%ss, %0" : "=r"(v)); r->ss = v; }
    { uint16_t v; asm volatile("movw %%ds, %0" : "=r"(v)); r->ds = v; }

    /* EIP = return address into our caller (kpanic / kpanic_at) */
    r->eip      = (uint32_t)(uintptr_t)__builtin_return_address(0);
    r->err_code = 0;
    r->int_no   = 0;
    r->useresp  = r->esp;
}

/* ============================================================
 * Public API
 * ============================================================ */

void kpanic(const char *msg)
{
    registers_t regs;
    capture_cpu_state(&regs);
    kernel_panic(NULL, msg, NULL, NULL, 0, 0, 0, &regs);
}

void kpanic_at(const char *msg, const char *file, const char *func, int line)
{
    registers_t regs;
    capture_cpu_state(&regs);
    kernel_panic(NULL, msg, file, func, line, 0, 0, &regs);
}

/* ============================================================
 * Exception handlers
 * ============================================================ */

static void debug_exception_handler(registers_t *regs)
{
    Serial_WriteString("[DEBUG] INT1 single-step\n");
    serial_dump(regs);
}

static void breakpoint_handler(registers_t *regs)
{
    Serial_WriteString("[DEBUG] INT3 breakpoint\n");
    serial_dump(regs);
}

static void double_fault_handler(registers_t *regs)
{
    kernel_panic("DOUBLE FAULT", NULL, NULL, NULL, 0, 0, 0, regs);
}

/* kill_userspace_fault -- if the faulting frame was in ring 3 (CS&3 == 3),
 * log the fault and terminate the offending task with the given signal so
 * the kernel can keep running.  Returns only when the offender cannot be
 * killed (unkillable task -- shells, idle): caller falls through to panic. */
static int kill_userspace_fault(const char *fault_type, int signo,
                                 registers_t *r, uint32_t fault_addr,
                                 int show_addr)
{
    if (!r || (r->cs & 3) != 3)
        return 0;  /* ring-0 fault -- real kernel bug, fall through to panic */

    task_t *t = task_current();
    if (!t || t->unkillable)
        return 0;  /* no task to blame, or it's the shell/idle: panic. */

    Serial_WriteString("[fault] ");
    Serial_WriteString((char *)fault_type);
    Serial_WriteString(" in pid=");
    Serial_WriteDec((uint32_t)t->pid);
    Serial_WriteString(" (");
    Serial_WriteString((char *)(t->name ? t->name : "?"));
    Serial_WriteString(") EIP=");
    ser_hex(r->eip);
    if (show_addr) {
        Serial_WriteString(" addr=");
        ser_hex(fault_addr);
    }
    Serial_WriteString(" err=");
    ser_hex(r->err_code);
    Serial_WriteString(" -- delivering SIG");
    Serial_WriteString(signo == SIGSEGV ? "SEGV" :
                       signo == SIGILL  ? "ILL"  :
                       signo == SIGFPE  ? "FPE"  : "?");
    Serial_WriteChar('\n');

    /* Linux wstatus convention: low 7 bits = signo for signal death. */
    t->exit_status = signo & 0x7F;
    task_exit();  /* noreturn -- scheduler picks another task */
}

static void gpf_handler(registers_t *regs)
{
    if (kill_userspace_fault("GENERAL PROTECTION FAULT", SIGSEGV, regs, 0, 0))
        return;  /* unreachable -- task_exit is noreturn */
    kernel_panic("GENERAL PROTECTION FAULT", NULL, NULL, NULL, 0, 0, 0, regs);
}

/* COW fault handler (slice 12c, fork+COW).
 *
 * Returns 1 if the fault was a legitimate copy-on-write fault that we
 * resolved by either flipping the PTE writable (sole owner) or by
 * allocating a fresh frame and memcpy'ing the page; 0 if this isn't a
 * COW fault and the caller should fall through to panic.
 *
 * Walks the currently-active PD via CR3 (which is the faulting task's
 * PD by construction -- the #PF was taken from its context). */
#define PF_ERR_PRESENT  0x1u
#define PF_ERR_WRITE    0x2u
#define PFE_PAGE_PRESENT  0x1u
#define PFE_PAGE_WRITABLE 0x2u
#define PFE_PAGE_LARGE    0x80u

/* Demand-paging handler (lazy brk + anonymous mmap).
 *
 * SYS_BRK and SYS_MMAP2 only *reserve* address ranges; the actual frames are
 * mapped here on first touch -- the WWLD model (Linux brk/anon-mmap are lazy).
 * This keeps a multi-MiB allocation (doom's 6 MiB zone) from mapping every
 * page in one interrupts-off syscall and stalling the whole system.
 *
 * Returns 1 if the fault hit a reserved-but-unmapped page in the current
 * task's heap [user_brk_base, user_brk) or anon-mmap [USER_MMAP_BASE,
 * mmap_next) window and we mapped a fresh zeroed frame; 0 otherwise (caller
 * falls through to the COW check / SIGSEGV). */
static int try_handle_anon_fault(uint32_t fault_addr, uint32_t err_code)
{
    /* A present-page fault here means a protection violation, not a missing
     * page -- leave it to COW / SIGSEGV. */
    if (err_code & PF_ERR_PRESENT)
        return 0;

    task_t *t = task_current();
    if (!t || t->user_brk == 0)
        return 0;                       /* not a user process */

    int in_heap = (t->user_brk_base && fault_addr >= t->user_brk_base &&
                   fault_addr <  t->user_brk);
    int in_mmap = (t->mmap_next && fault_addr >= USER_MMAP_BASE &&
                   fault_addr <  t->mmap_next);
    if (!in_heap && !in_mmap)
        return 0;                       /* genuine wild access */

    uint32_t phys = pmm_alloc_frame();
    if (phys == PMM_ALLOC_ERROR)
        return 0;                       /* OOM: fall through to SIGSEGV/panic */

    /* Frame is kernel-identity-mapped (< 256 MiB) so we can zero it directly. */
    memset((void *)phys, 0, 0x1000u);
    vmm_map_page(t->page_dir, fault_addr & ~0xFFFu, phys,
                 VMM_FLAG_USER | VMM_FLAG_WRITABLE);
    asm volatile("invlpg (%0)" :: "r"(fault_addr & ~0xFFFu) : "memory");
    return 1;
}

static int try_handle_cow_fault(uint32_t fault_addr, uint32_t err_code)
{
    /* Not a write fault?  Can't be COW.  (A read against a present
     * RO+COW page wouldn't fault on i386 anyway -- RO blocks writes,
     * not reads.) */
    if (!(err_code & PF_ERR_WRITE) || !(err_code & PF_ERR_PRESENT))
        return 0;

    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint32_t *pd = (uint32_t *)(cr3 & ~0xFFFu);

    uint32_t pdi = fault_addr >> 22;
    uint32_t pde = pd[pdi];
    if (!(pde & PFE_PAGE_PRESENT) || (pde & PFE_PAGE_LARGE))
        return 0;

    uint32_t *pt  = (uint32_t *)(pde & ~0xFFFu);
    uint32_t pti  = (fault_addr >> 12) & 0x3FFu;
    uint32_t pte  = pt[pti];

    if (!(pte & PFE_PAGE_PRESENT) || !(pte & VMM_PTE_COW))
        return 0;

    uint32_t frame  = pte & ~0xFFFu;
    uint32_t vbase  = fault_addr & ~0xFFFu;
    uint32_t flags  = pte & 0xFFFu;

    uint8_t rc = pmm_ref_count(frame);

    if (rc <= 1) {
        /* Sole owner -- no copy needed, just promote back to writable.
         * (rc==1 is the normal case; rc==0 would indicate a refcount
         * bug, but defensively we still flip the PTE so the task can
         * make forward progress rather than panicking.) */
        pt[pti] = (frame) | (((flags | PFE_PAGE_WRITABLE) & ~VMM_PTE_COW));
    } else {
        /* Shared frame -- allocate a private copy. */
        uint32_t new_frame = pmm_alloc_frame();
        if (new_frame == PMM_ALLOC_ERROR)
            return 0;  /* OOM: fall through to panic */

        /* Both frames are kernel-identity-mapped (< 256 MiB), so we
         * can memcpy via their physical addresses directly. */
        memcpy((void *)new_frame, (void *)frame, 0x1000);

        /* Drop our reference to the shared frame; if other tasks
         * still hold it the refcount just decrements. */
        pmm_free_frame(frame);

        pt[pti] = new_frame | (((flags | PFE_PAGE_WRITABLE) & ~VMM_PTE_COW));
    }

    /* Flush the stale RO entry from the TLB. */
    asm volatile("invlpg (%0)" :: "r"(vbase) : "memory");
    return 1;
}

static void page_fault_handler(registers_t *regs)
{
    uint32_t fault_addr;
    asm volatile("mov %%cr2, %0" : "=r"(fault_addr));

    if (try_handle_anon_fault(fault_addr, regs->err_code))
        return;

    if (try_handle_cow_fault(fault_addr, regs->err_code))
        return;

    /* Ring-3 page fault: a userspace bug (null deref, stack overflow,
     * unmapped access).  Don't take the kernel down with it -- log the
     * fault, deliver SIGSEGV, and let the scheduler reap the offender.
     * Ring-0 faults are real kernel bugs and still panic. */
    if (kill_userspace_fault("PAGE FAULT", SIGSEGV, regs, fault_addr, 1))
        return;  /* unreachable -- task_exit is noreturn */

    kernel_panic("PAGE FAULT", NULL, NULL, NULL, 0, fault_addr, 1, regs);
}

/* ============================================================
 * Initialisation
 * ============================================================ */

void init_debug_handlers(void)
{
    register_interrupt_handler(1,  debug_exception_handler);
    register_interrupt_handler(3,  breakpoint_handler);
    register_interrupt_handler(8,  double_fault_handler);
    register_interrupt_handler(13, gpf_handler);
    register_interrupt_handler(14, page_fault_handler);
}
