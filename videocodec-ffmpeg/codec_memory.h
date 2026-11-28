/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * codec_memory.h - the memory the frames' pictures live in, within the VPU's reach (codec_memory.c).
 *
 * A header of its own: codec_memory.c takes emu68-common's DMA pool, whose
 * memory.h declares the string functions its own way, and must not see the
 * libnix headers FFmpeg's bring in.
 */
#ifndef CODEC_MEMORY_H
#define CODEC_MEMORY_H

#include <exec/types.h>

#define MEMORY_ALIGN 64u /* a picture starts on a cache line: the VPU imports whole lines */

void memory_open(void);
void memory_close(void);
APTR memory_alloc(ULONG size);
void memory_free(APTR memory);

#endif /* CODEC_MEMORY_H */
