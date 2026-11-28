/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * codec_memory.c - the memory the frames' pictures live in: where the VPU reaches.
 *
 * A converter behind the decoder (videocore.codec's ISP) imports the frames'
 * memory and reads it itself, and the VPU addresses only the lowest GiB of the
 * Pi's RAM (videocodec-library's architecture, section 5.6).  Plain Fast RAM
 * lies there only while it is not full: with a large file in RAM: it comes
 * from above, the import fails and so does the session.  So the pictures come
 * from emu68-common's DMA pool over the regions of Emu68's RAM wholly below
 * the reach, as rpivid.codec's do.  A region is a MemHeader the pool
 * allocates anywhere in, so one that crosses the reach is left out whole.
 * Memory beyond the reach is never taken instead: a converter could not
 * import it, so a picture without room below the reach fails the session.
 *
 * The pool takes Forbid() for itself: the sets are made in the sessions'
 * processes and given back from the clients' tasks too.
 */
#include <proto/exec.h>

#include <dma_mem.h>

#include "codec_memory.h"

#define VPU_REACH_END 0x40000000u /* the VPU addresses the lowest GiB (Pi 4): the pictures lie below */

static struct dma_mem_ctx regions; /* Emu68's RAM below the reach; the pool's, for its life */
static struct dma_pool *pool;      /* the pictures' pool, NULL when no region lies below the reach */

/**
 * memory_open() - Make the pool over the regions of Emu68's RAM wholly below the VPU's reach; the library's init calls it.
 *
 * Without such a region there is no pool, and no picture gets memory.
 */
void memory_open(void)
{
	dma_mem_init(&regions, SysBase);
	ULONG kept = 0;
	for (ULONG i = 0; i < regions.count; i++)
		if (regions.regions[i].end <= VPU_REACH_END)
			regions.regions[kept++] = regions.regions[i];
	regions.count = kept;
	pool = kept != 0 ? dma_pool_create(&regions) : NULL;
}

/**
 * memory_close() - Give the pool back; the library's cleanup calls it, every set given back.
 */
void memory_close(void)
{
	if (pool != NULL)
		dma_pool_delete(pool);
	pool = NULL;
}

/**
 * memory_alloc() - Memory for a set's pictures, on a cache line and within the VPU's reach.
 * @size: its bytes
 *
 * Return: the memory, NULL when there is no room below the reach (or no pool); give it back with memory_free().
 */
APTR memory_alloc(ULONG size)
{
	return pool != NULL ? dma_alloc(pool, MEMORY_ALIGN, size) : NULL;
}

/**
 * memory_free() - Give back what memory_alloc() gave.
 * @memory: the memory; NULL is passed over
 */
void memory_free(APTR memory)
{
	if (memory != NULL && pool != NULL)
		dma_free(pool, memory);
}
