/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * amiga_library.h - libnix and FFmpeg inside an Amiga shared library.
 *
 * libnix's library start-up (-ramiga-lib) gives the resident tag and the
 * standard vectors.  What a program's start-up code does besides, the library
 * does itself through these: libnix's own init and exit functions, the
 * libraries it opens for the code that uses their bases, and a place for
 * exit() and abort() to land, since there is no program to end.
 *
 * FFmpeg is built without threads and must have one caller at a time, so
 * every call into it sits between FFMPEG_ENTER() and ffmpeg_leave():
 *
 *   if (!FFMPEG_ENTER())
 *   {
 *       ffmpeg_leave();
 *       return ERROR;        // FFmpeg called exit() or abort()
 *   }
 *   ... calls into FFmpeg ...
 *   ffmpeg_leave();
 */
#ifndef KINORA_FFMPEG_AMIGA_LIBRARY_H
#define KINORA_FFMPEG_AMIGA_LIBRARY_H

#include <setjmp.h>
#include <exec/types.h>

/* Where exit() and abort() land while a call is inside FFmpeg */
extern jmp_buf ffmpeg_escape;

/* Take FFmpeg for this caller; the escape is armed by FFMPEG_ENTER()'s setjmp() */
void ffmpeg_lock(void);
/* Give FFmpeg back and disarm the escape */
void ffmpeg_leave(void);

/* TRUE on the way in, FALSE when exit() or abort() came back here; setjmp() must run in the caller's frame */
#define FFMPEG_ENTER() (ffmpeg_lock(), setjmp(ffmpeg_escape) == 0)

/* The library's own init and cleanup, called from libnix's hooks with libnix up */
BOOL library_open(void);
void library_close(void);

/*
 * A profile-generating build (KINORA_PGO GENERATE, docs/architecture.md section 3) has one
 * private call after the public ones: PGO_Dump(), which writes the profile kept so far as one
 * gcda stream (pgo_dump.c), called by the PgoDump tool from the training rig's process.  A
 * library's function table ends with the macro, within the line of its last entry:
 *
 *   (APTR)ACB_Flush, KINORA_PGO_DUMP_ENTRY (APTR)-1,
 */
#ifdef KINORA_PGO_GENERATE
LONG PGO_Dump(STRPTR path asm("a0"));
#define KINORA_PGO_DUMP_ENTRY (APTR)PGO_Dump,
#else
#define KINORA_PGO_DUMP_ENTRY
#endif

#endif /* KINORA_FFMPEG_AMIGA_LIBRARY_H */
