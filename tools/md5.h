/* SPDX-License-Identifier: MPL-2.0 */
/*
 * md5.h - MD5 (RFC 1321), for VCDecode's digest of each picture.
 */
#ifndef KINORA_TOOLS_MD5_H
#define KINORA_TOOLS_MD5_H

#include <exec/types.h>

#define MD5_BYTES 16
#define MD5_BLOCK 64

/* A digest being made */
struct md5
{
	ULONG state[4];         /* A, B, C, D */
	ULONG length_low;       /* bytes taken, low 32 bits */
	ULONG length_high;      /* and high */
	UBYTE block[MD5_BLOCK]; /* the block being filled */
	ULONG used;             /* its bytes */
};

void md5_start(struct md5 *md5);
void md5_add(struct md5 *md5, const UBYTE *data, ULONG length);
void md5_end(struct md5 *md5, UBYTE digest[MD5_BYTES]);

#endif
