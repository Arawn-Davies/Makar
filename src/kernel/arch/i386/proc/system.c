#include <kernel/system.h>
#include <kernel/types.h>
#include <kernel/tty.h>

inline void io_wait()
{
    /* TODO: This is probably fragile. */
    asm volatile ( "jmp 1f\n\t"
                   "1:jmp 2f\n\t"
                   "2:" );
}

//Halts the CPU, executes when there's an unrecoverable exception or some other error
inline void halt()
{
	asm volatile ("hlt");
}

void panic(char* message, char* file, uint32_t line)
{
	asm volatile ("cli");
	t_writestring("\nPANIC(");
	t_writestring(message);
	t_writestring(") at ");
	t_writestring(file);
	t_writestring(":");
	t_dec(line);
	t_putchar('\n');
	for(;;);
}

void panic_assert(char *file, uint32_t line, char *desc)
{
	asm volatile("cli");

	t_writestring("ASSERTION-FAILED(");
	t_writestring(desc);
	t_writestring(") at ");
	t_writestring(file);
	t_writestring(":");
	t_dec(line);
	t_putchar('\n');
	for(;;);
}
