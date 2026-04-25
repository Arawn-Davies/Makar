/*
 * debug.c — exception handlers for INT 1, 3, 8, 13, 14.
 *
 * All fatal handlers (8, 13, 14) write to serial first, unconditionally,
 * because serial uses port I/O and is safe regardless of CR3.  VGA is
 * attempted afterwards only when the kernel page directory is active;
 * skipping it avoids a recursive page fault if we took the exception
 * while a user PD was loaded and the VESA framebuffer wasn't mapped.
 */

#include <kernel/debug.h>
#include <kernel/isr.h>
#include <kernel/paging.h>
#include <kernel/tty.h>
#include <kernel/serial.h>

/* True when the kernel page directory is currently loaded in CR3.
   VGA (VESA framebuffer) is only safely accessible in that case. */
static int kernel_pd_active(void)
{
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return (cr3 == (uint32_t)paging_kernel_pd());
}

// ---------------------------------------------------------------------------
// Serial-safe register dump (always works, regardless of CR3)
// ---------------------------------------------------------------------------

static void serial_reg(const char *name, uint32_t val)
{
    Serial_WriteString(name);
    Serial_WriteString("=");
    Serial_WriteHex(val);
    Serial_WriteString("\n");
}

static void serial_dump(registers_t *regs)
{
    Serial_WriteString("--- register dump ---\n");
    serial_reg("EIP   ", regs->eip);
    serial_reg("EFLAGS", regs->eflags);
    serial_reg("EAX   ", regs->eax);
    serial_reg("EBX   ", regs->ebx);
    serial_reg("ECX   ", regs->ecx);
    serial_reg("EDX   ", regs->edx);
    serial_reg("ESI   ", regs->esi);
    serial_reg("EDI   ", regs->edi);
    serial_reg("EBP   ", regs->ebp);
    serial_reg("ESP   ", regs->esp);
    serial_reg("CS    ", regs->cs);
    serial_reg("SS    ", regs->ss);
    Serial_WriteString("---------------------\n");
}

/* VGA mirror — only called after kernel_pd_active() returns true. */
static void vga_dump(registers_t *regs)
{
    t_writestring("\n--- register dump ---\n");
    #define VR(n, v) do { t_writestring(n); t_writestring("="); t_hex(v); t_writestring("\n"); } while(0)
    VR("EIP   ", regs->eip);
    VR("EFLAGS", regs->eflags);
    VR("EAX   ", regs->eax);
    VR("EBX   ", regs->ebx);
    VR("ECX   ", regs->ecx);
    VR("EDX   ", regs->edx);
    VR("ESI   ", regs->esi);
    VR("EDI   ", regs->edi);
    VR("EBP   ", regs->ebp);
    VR("ESP   ", regs->esp);
    VR("CS    ", regs->cs);
    VR("SS    ", regs->ss);
    #undef VR
    t_writestring("---------------------\n");
}

// ---------------------------------------------------------------------------
// INT 1 – Debug exception (single-step / hardware breakpoint)
// ---------------------------------------------------------------------------

static void debug_exception_handler(registers_t *regs)
{
    Serial_WriteString("[DEBUG] INT1 debug exception\n");
    serial_dump(regs);
    if (kernel_pd_active()) {
        t_writestring("[DEBUG] INT1 debug exception\n");
        vga_dump(regs);
    }
}

// ---------------------------------------------------------------------------
// INT 3 – Breakpoint
// ---------------------------------------------------------------------------

static void breakpoint_handler(registers_t *regs)
{
    Serial_WriteString("[DEBUG] INT3 breakpoint hit\n");
    serial_dump(regs);
    if (kernel_pd_active()) {
        t_writestring("[DEBUG] INT3 breakpoint hit\n");
        vga_dump(regs);
    }
}

// ---------------------------------------------------------------------------
// INT 8 – Double Fault
// ---------------------------------------------------------------------------

static void double_fault_handler(registers_t *regs)
{
    Serial_WriteString("\n*** DOUBLE FAULT (err=");
    Serial_WriteHex(regs->err_code);
    Serial_WriteString(") — halted ***\n");
    serial_dump(regs);
    if (kernel_pd_active()) {
        t_writestring("\n*** DOUBLE FAULT (err=0x");
        t_hex(regs->err_code);
        t_writestring(") — system halted ***\n");
        vga_dump(regs);
    }
    for (;;) asm volatile("cli; hlt");
}

// ---------------------------------------------------------------------------
// INT 13 – General Protection Fault
// ---------------------------------------------------------------------------

static void gpf_handler(registers_t *regs)
{
    Serial_WriteString("\n*** GPF (err=");
    Serial_WriteHex(regs->err_code);
    Serial_WriteString(") — halted ***\n");
    serial_dump(regs);
    if (kernel_pd_active()) {
        t_writestring("\n*** GENERAL PROTECTION FAULT (err=0x");
        t_hex(regs->err_code);
        t_writestring(") — system halted ***\n");
        vga_dump(regs);
    }
    for (;;) asm volatile("cli; hlt");
}

// ---------------------------------------------------------------------------
// INT 14 – Page Fault
//
// Error code bits: 0=P(present), 1=W/R, 2=U/S, 3=RSVD, 4=I/D
// CR2 holds the faulting linear address.
// ---------------------------------------------------------------------------

static void page_fault_handler(registers_t *regs)
{
    uint32_t fault_addr;
    asm volatile("mov %%cr2, %0" : "=r"(fault_addr));

    /* Serial first — always safe. */
    Serial_WriteString("\n*** PAGE FAULT @ ");
    Serial_WriteHex(fault_addr);
    Serial_WriteString(" err=");
    Serial_WriteHex(regs->err_code);
    Serial_WriteString(" [");
    Serial_WriteString((regs->err_code & 0x1) ? "PROT"   : "NP");
    Serial_WriteString((regs->err_code & 0x2) ? " WRITE" : " READ");
    Serial_WriteString((regs->err_code & 0x4) ? " USER"  : " KERN");
    if (regs->err_code & 0x8)  Serial_WriteString(" RSVD");
    if (regs->err_code & 0x10) Serial_WriteString(" IFETCH");
    Serial_WriteString("] — halted ***\n");
    serial_dump(regs);

    /* VGA only when we know the kernel PD (and VESA mapping) is active. */
    if (kernel_pd_active()) {
        t_writestring("\n*** PAGE FAULT @ 0x");
        t_hex(fault_addr);
        t_writestring(" err=0x");
        t_hex(regs->err_code);
        t_writestring(" [");
        t_writestring((regs->err_code & 0x1) ? "PROT"   : "NP");
        t_writestring((regs->err_code & 0x2) ? " WRITE" : " READ");
        t_writestring((regs->err_code & 0x4) ? " USER"  : " KERN");
        if (regs->err_code & 0x8)  t_writestring(" RSVD");
        if (regs->err_code & 0x10) t_writestring(" IFETCH");
        t_writestring("] — system halted ***\n");
        vga_dump(regs);
    }

    for (;;) asm volatile("cli; hlt");
}

// ---------------------------------------------------------------------------
// Public initialisation – called from kernel_main
// ---------------------------------------------------------------------------

void init_debug_handlers()
{
    register_interrupt_handler(1,  debug_exception_handler);
    register_interrupt_handler(3,  breakpoint_handler);
    register_interrupt_handler(8,  double_fault_handler);
    register_interrupt_handler(13, gpf_handler);
    register_interrupt_handler(14, page_fault_handler);
}
