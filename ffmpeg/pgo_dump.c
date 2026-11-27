/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * pgo_dump.c - the profile of a profile-generating build (KINORA_PGO GENERATE), written by
 * the library itself (docs/architecture.md, section 3).
 *
 * libgcov's own writer finds no file to write under libnix and says nothing, so this takes
 * its place: __gcov_init() (called by each instrumented object's constructor) only keeps the
 * object's record, __gcov_exit() does nothing, and PGO_Dump() writes every record as one
 * stream (__gcov_info_to_gcda() with __gcov_filename_to_gcfn()), which the host's
 * `gcov-tool merge-stream` turns into the objects' .gcda files.  Linked only into such builds.
 */
#include <gcov.h>
#include <stdio.h>
#include <stdlib.h>

#include "amiga_library.h"

#define PGO_UNITS_MAX 2048 /* instrumented objects: FFmpeg's video build has about 300 */

static const struct gcov_info *pgo_units[PGO_UNITS_MAX];
static unsigned pgo_unit_count;

void __gcov_init(struct gcov_info *info);
void __gcov_exit(void);
long long __gcov_read_counter(void);

/**
 * __gcov_read_counter() - Stands in for libgcov's, so that its file code stays out of the link.
 *
 * Every record names __gcov_merge_add(), which reads counters only when merging into a file
 * on the Amiga; PGO_Dump() never does (gcov-tool merges on the host), so this is never called.
 *
 * Return: 0.
 */
long long __gcov_read_counter(void)
{
	return 0;
}

/**
 * __gcov_init() - Keep an instrumented object's record (its constructor calls this).
 * @info: the record
 */
void __gcov_init(struct gcov_info *info)
{
	if (pgo_unit_count < PGO_UNITS_MAX)
		pgo_units[pgo_unit_count++] = info;
}

/**
 * __gcov_exit() - Nothing at the expunge: PGO_Dump() writes the records.
 */
void __gcov_exit(void)
{
}

/**
 * pgo_write() - Append a piece of the stream.
 * @data:   the bytes
 * @length: how many
 * @arg:    the stream's file
 */
static void pgo_write(const void *data, unsigned length, void *arg)
{
	fwrite(data, 1, length, (FILE *)arg);
}

/**
 * pgo_filename() - Name the record's .gcda file in the stream.
 * @name: the file's name as compiled in
 * @arg:  the stream's file
 */
static void pgo_filename(const char *name, void *arg)
{
	__gcov_filename_to_gcfn(name, pgo_write, arg);
}

/**
 * pgo_allocate() - Memory __gcov_info_to_gcda() asks for.
 * @length: bytes
 * @arg:    unused
 *
 * Return: the memory.
 */
static void *pgo_allocate(unsigned length, void *arg)
{
	(void)arg;
	return malloc(length);
}

/**
 * PGO_Dump() - The private call: write every record kept so far as one stream.
 * @path: the stream's file (A0)
 *
 * Called from the PgoDump tool's process, not in an expunge: the file is written in the
 * caller's context, outside Forbid().
 *
 * Return: how many records, or -1 when the file cannot be made.
 */
LONG PGO_Dump(STRPTR path asm("a0"))
{
	FILE *out = fopen((const char *)path, "wb");
	if (out == NULL)
		return -1;
	for (unsigned i = 0; i < pgo_unit_count; i++)
		__gcov_info_to_gcda(pgo_units[i], pgo_filename, pgo_write, pgo_allocate, out);
	fclose(out);
	return (LONG)pgo_unit_count;
}
