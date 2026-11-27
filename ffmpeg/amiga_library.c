/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * amiga_library.c - libnix and FFmpeg inside an Amiga shared library
 * (amiga_library.h has the why).
 */
#include <stdlib.h>
#include <exec/execbase.h>
#include <exec/semaphores.h>
#include <proto/exec.h>

#include "amiga_library.h"

#define LIST_PRIORITIES 256     /* an init or exit function's priority, plus 128: 0 to 255 */
#define LIBRARY_UNOPENED (-1L)  /* a library list slot before its library is open */

int __UserLibInit(struct Library *library, APTR data);
void __UserLibCleanup(APTR data);

struct ExecBase *SysBase; /* a program's start-up code defines it; here nothing else does */

/*
 * The heads of libnix's lists, which a program gets from its start-up code: a zero word at the start of each list's
 * section, and one after the last section.  They are made in assembly and declared without a size, because the
 * entries behind a head belong to other objects: the compiler, shown a one-word array holding zero, is free to
 * decide that nothing follows it, and then no init function is ever called.
 *
 * The linker sorts these sections by name, which puts the library's function table between the exit list and the
 * init list: the exit list gets an end of its own, in a section that sorts right behind it.
 */
/* A list's head: the symbol and a zero word, in the section the list's entries follow in */
#define LIST_HEAD(section, flags, symbol) \
	__asm__(".section " section ",\"" flags "\"\n" \
	        ".globl " symbol "\n" \
	        symbol ":\n" \
	        ".long 0\n" \
	        ".text\n")
/* A list's end: a zero word in a section that sorts behind the list's entries */
#define LIST_END(section, flags) \
	__asm__(".section " section ",\"" flags "\"\n" \
	        ".long 0\n" \
	        ".text\n")

LIST_HEAD(".list___INIT_LIST__", "a", "___INIT_LIST__");
LIST_HEAD(".list___EXIT_LIST__", "a", "___EXIT_LIST__");
LIST_END(".list___EXIT_LIST__end", "a");
LIST_END(".end_of_lists", "a");
LIST_HEAD(".dlist___LIB_LIST__", "aw", "___LIB_LIST__");
LIST_END(".end_of_dlists", "aw");
#ifdef KINORA_PGO_GENERATE
/*
 * The heads of libnix's constructor and destructor lists, which the profiling code of a
 * profile-generating build registers through (each instrumented object's constructor calls
 * __gcov_init()).  gcc puts constructors in .list___CTOR_LIST__.<priority> and destructors in
 * .list___DTOR_LIST__.<priority>; libnix's __initcpp() and __exitcpp(), entries of the init
 * and exit lists, walk them, but only a program's start-up defines the heads.  Each list ends
 * at the next section's head, a zero word: the destructors' at the exit list's.  The link
 * asks for __initcpp (-u ___initcpp), which nothing else pulls in.
 */
LIST_HEAD(".list___CTOR_LIST__", "a", "___CTOR_LIST__");
LIST_HEAD(".list___DTOR_LIST__", "a", "___DTOR_LIST__");
#endif
extern const long __INIT_LIST__[];
extern const long __EXIT_LIST__[];
extern long __LIB_LIST__[];

jmp_buf ffmpeg_escape;

static struct SignalSemaphore ffmpeg_semaphore; /* FFmpeg's one caller at a time */
static BOOL escape_armed;                       /* a call is inside FFmpeg (or libnix's init), so exit() has a place to land */

/**
 * exit() - What libnix's exit() does in a library: fail the call that is inside FFmpeg.
 * @code: the exit code, of no use here
 *
 * libnix's own belongs to the start-up code a library does not have.  With no
 * call to fail the task has nowhere to go and waits for good.
 */
void exit(int code)
{
	(void)code;
	if (escape_armed)
		longjmp(ffmpeg_escape, 1);
	for (;;)
		Wait(0);
}

/**
 * abort() - As exit(): FFmpeg's failed assertions end here.
 */
void abort(void)
{
	exit(EXIT_FAILURE);
}

/**
 * ffmpeg_lock() - Take FFmpeg for this caller and arm the escape.
 */
void ffmpeg_lock(void)
{
	ObtainSemaphore(&ffmpeg_semaphore);
	escape_armed = TRUE;
}

/**
 * ffmpeg_leave() - Give FFmpeg back and disarm the escape.
 */
void ffmpeg_leave(void)
{
	escape_armed = FALSE;
	ReleaseSemaphore(&ffmpeg_semaphore);
}

/**
 * list_call() - Call the functions of one of libnix's lists in the order of their priorities.
 * @list:       the list's head: pairs of a function and its priority plus 128 follow, up to a zero
 * @descending: highest priority first (the exit list); else lowest first (the init list)
 */
static void list_call(const long *list, BOOL descending)
{
	for (long pass = 0; pass < LIST_PRIORITIES; pass++)
	{
		long priority = descending ? LIST_PRIORITIES - 1 - pass : pass;
		for (const long *entry = list + 1; entry[0] != 0; entry += 2)
			if (entry[1] == priority)
				((void (*)(void))entry[0])();
	}
}

/**
 * libraries_close() - Close what libraries_open() opened.
 */
static void libraries_close(void)
{
	for (long *entry = __LIB_LIST__ + 1; entry[0] != 0; entry += 2)
	{
		if (entry[0] != LIBRARY_UNOPENED)
			CloseLibrary((struct Library *)entry[0]);
		entry[0] = LIBRARY_UNOPENED;
	}
}

/**
 * libraries_open() - Open the libraries libnix opens for the code that uses their bases.
 *
 * Each entry of the list is the base variable itself, LIBRARY_UNOPENED at
 * first, and the library's name.
 *
 * Return: TRUE with all of them open, FALSE with none.
 */
static BOOL libraries_open(void)
{
	for (long *entry = __LIB_LIST__ + 1; entry[0] != 0; entry += 2)
	{
		struct Library *opened = OpenLibrary((STRPTR)entry[1], 0);
		if (opened == NULL)
		{
			libraries_close();
			return FALSE;
		}
		entry[0] = (long)opened;
	}
	return TRUE;
}

/**
 * __UserLibInit() - libnix's hook from the library's init: bring libnix up, then the library itself.
 * @library: the library base
 * @data:    the data segment, unused: the library has one copy of its data
 *
 * Return: 0, or 1 to refuse the open.
 */
int __UserLibInit(struct Library *library, APTR data)
{
	(void)library;
	(void)data;
	SysBase = *(struct ExecBase **)4;
	InitSemaphore(&ffmpeg_semaphore);
	if (!libraries_open())
		return 1;

	volatile BOOL up = FALSE; /* set after setjmp(), read after a longjmp() */
	if (FFMPEG_ENTER())
	{
		list_call(__INIT_LIST__, FALSE);
		up = library_open();
	}
	ffmpeg_leave();
	if (!up)
		libraries_close();
	return up ? 0 : 1;
}

/**
 * __UserLibCleanup() - libnix's hook from the expunge: the library's own cleanup, then libnix down.
 * @data: the data segment, unused
 */
void __UserLibCleanup(APTR data)
{
	(void)data;
	library_close();
	list_call(__EXIT_LIST__, TRUE);
	libraries_close();
}
