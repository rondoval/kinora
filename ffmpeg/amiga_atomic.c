/*
 * The atomic helpers gcc leaves to libatomic, which m68k-amigaos does not have.
 * FFmpeg uses them for reference counts. One 68k runs the tasks, so keeping the
 * scheduler away for the three instructions is enough.
 */
#include <proto/exec.h>

unsigned __atomic_fetch_add_4(volatile void *pointer, unsigned value, int order);
unsigned __atomic_fetch_sub_4(volatile void *pointer, unsigned value, int order);
unsigned __atomic_exchange_4(volatile void *pointer, unsigned value, int order);

/**
 * __atomic_fetch_add_4() - Add to a 32-bit word and say what it held.
 * @pointer: the word
 * @value:   what to add
 * @order:   the memory order asked for, of no matter on one processor
 *
 * Return: the word before.
 */
unsigned __atomic_fetch_add_4(volatile void *pointer, unsigned value, int order)
{
	volatile unsigned *word = pointer;
	(void)order;

	Forbid();
	unsigned old = *word;
	*word = old + value;
	Permit();
	return old;
}

/**
 * __atomic_fetch_sub_4() - Subtract from a 32-bit word and say what it held.
 * @pointer: the word
 * @value:   what to subtract
 * @order:   the memory order asked for, of no matter on one processor
 *
 * Return: the word before.
 */
unsigned __atomic_fetch_sub_4(volatile void *pointer, unsigned value, int order)
{
	volatile unsigned *word = pointer;
	(void)order;

	Forbid();
	unsigned old = *word;
	*word = old - value;
	Permit();
	return old;
}

/**
 * __atomic_exchange_4() - Put a value into a 32-bit word and say what it held.
 * @pointer: the word
 * @value:   the new value
 * @order:   the memory order asked for, of no matter on one processor
 *
 * Return: the word before.
 */
unsigned __atomic_exchange_4(volatile void *pointer, unsigned value, int order)
{
	volatile unsigned *word = pointer;
	(void)order;

	Forbid();
	unsigned old = *word;
	*word = value;
	Permit();
	return old;
}
