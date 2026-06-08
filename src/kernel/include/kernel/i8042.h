#ifndef KERNEL_I8042_H
#define KERNEL_I8042_H

/*
 * i8042 -- shared PS/2 controller byte router.
 *
 * The 8042 multiplexes the keyboard (IRQ1) and the AUX/mouse channel (IRQ12)
 * onto one output buffer.  Both IRQ handlers call i8042_service(): it drains the
 * output buffer and dispatches each byte by its AUXB status bit to the keyboard
 * decoder or the mouse packet assembler.  Keeping the dispatch here means the
 * keyboard and mouse drivers depend on the controller, not on each other.
 */
void i8042_service(void);

#endif /* KERNEL_I8042_H */
