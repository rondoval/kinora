/* SPDX-License-Identifier: MPL-2.0 */
/*
 * PgoDump - have a profile-generating library write its profile (docs/architecture.md,
 * section 3), for the training round (scripts/pgo-train.py).
 *
 *   PgoDump LIBRARY/A,FUNCTIONS/A/N,TO/A
 *
 * LIBRARY is opened by name, so the copy in memory is the one asked, with the counts it has
 * gathered since it was loaded (demuxer.library, ffmpegaudio.codec, ffmpegvideo.codec).  Its
 * private call comes after its FUNCTIONS public ones; a library without it (a build that does
 * not generate profiles) is refused.  The call writes the profile to TO as one gcda stream
 * and returns how many instrumented objects it holds.  Built in profile-generating builds only.
 */
#include <stdio.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define LVO_FIRST 30 /* the first function's offset below the base: after the four standard vectors */
#define LVO_STEP 6   /* each vector's bytes */

static const char version_cookie[] __attribute__((used)) = "$VER: PgoDump " KINORA_VERSION_TEXT " " KINORA_DATE;
static const char template[] = "LIBRARY/A,FUNCTIONS/A/N,TO/A";

enum
{
	ARG_LIBRARY,
	ARG_FUNCTIONS,
	ARG_TO,
	ARG_COUNT
};

/**
 * dump_call() - Call a library's private dump vector.
 * @base:   the library
 * @offset: the vector's offset below the base
 * @path:   the stream's file (A0)
 *
 * Return: what the call returned: the objects written, or -1.
 */
static LONG dump_call(struct Library *base, LONG offset, const char *path)
{
	register struct Library *a6 __asm("a6") = base;
	register const char *a0 __asm("a0") = path;
	register LONG d0 __asm("d0");
	register APTR vector __asm("a1") = (APTR)((UBYTE *)base - offset);
	__asm__ volatile("jsr (%%a1)" : "=r"(d0), "+r"(vector), "+r"(a0) : "r"(a6) : "d1", "memory", "cc"); /* d0-d1/a0-a1 are the callee's */
	return d0;
}

/**
 * main() - Open the library, check it has the private call, make it.
 *
 * Return: RETURN_OK, RETURN_FAIL without the library, the call or the file.
 */
int main(void)
{
	LONG args[ARG_COUNT] = {0};
	struct RDArgs *parsed = ReadArgs((CONST_STRPTR)template, args, NULL);
	if (parsed == NULL)
	{
		PrintFault(IoErr(), (CONST_STRPTR) "PgoDump");
		return RETURN_FAIL;
	}
	int result = RETURN_FAIL;
	LONG offset = LVO_FIRST + LVO_STEP * *(LONG *)args[ARG_FUNCTIONS];
	struct Library *base = OpenLibrary((CONST_STRPTR)args[ARG_LIBRARY], 0);
	if (base == NULL)
		printf("%s did not open: is it in memory?\n", (const char *)args[ARG_LIBRARY]);
	else if (base->lib_NegSize < offset) /* the vector at -offset fills offset - LVO_STEP .. offset below the base */
		printf("%s has no private call: not a profile-generating build\n", (const char *)args[ARG_LIBRARY]);
	else
	{
		LONG written = dump_call(base, offset, (const char *)args[ARG_TO]);
		printf("dump=%ld to=%s\n", (long)written, (const char *)args[ARG_TO]);
		result = written > 0 ? RETURN_OK : RETURN_FAIL;
	}
	if (base != NULL)
		CloseLibrary(base);
	FreeArgs(parsed);
	return result;
}
