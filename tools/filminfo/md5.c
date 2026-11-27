/* SPDX-License-Identifier: MPL-2.0 */
/*
 * md5.c - MD5 as RFC 1321 describes it: four rounds of sixteen steps over each 64-byte
 * block, the message padded with a one bit, zeroes and its length in bits.
 */
#include <string.h>

#include "md5.h"

#define LENGTH_AT 56 /* where the length goes in the last block */
#define PAD_FIRST 0x80u
#define WORDS 16
#define STEPS 64

/* The constants of the 64 steps: the integer part of 2^32 times |sin(i + 1)| */
static const ULONG step_constant[STEPS] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

/* Each round's four rotations */
static const UBYTE step_shift[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};

/**
 * word_at() - A little-endian word of a block.
 * @block: the block
 * @index: the word, 0 to 15
 *
 * Return: the word.
 */
static ULONG word_at(const UBYTE *block, ULONG index)
{
	const UBYTE *at = block + 4 * index;
	return (ULONG)at[0] | ((ULONG)at[1] << 8) | ((ULONG)at[2] << 16) | ((ULONG)at[3] << 24);
}

/**
 * step_mix() - A step's function of B, C and D, and the word it takes.
 * @step:  0 to 63
 * @b:     B
 * @c:     C
 * @d:     D
 * @index: gets the message word the step adds
 *
 * Return: F, G, H or I of the step's round.
 */
static ULONG step_mix(ULONG step, ULONG b, ULONG c, ULONG d, ULONG *index)
{
	switch (step / WORDS)
	{
	case 0:
		*index = step;
		return (b & c) | (~b & d);
	case 1:
		*index = (5 * step + 1) % WORDS;
		return (b & d) | (c & ~d);
	case 2:
		*index = (3 * step + 5) % WORDS;
		return b ^ c ^ d;
	default:
		*index = (7 * step) % WORDS;
		return c ^ (b | ~d);
	}
}

/**
 * block_take() - Mix one 64-byte block into the state.
 * @md5:   the digest
 * @block: the block
 */
static void block_take(struct md5 *md5, const UBYTE *block)
{
	ULONG a = md5->state[0], b = md5->state[1], c = md5->state[2], d = md5->state[3];
	for (ULONG step = 0; step < STEPS; step++)
	{
		ULONG index;
		ULONG mixed = step_mix(step, b, c, d, &index) + a + step_constant[step] + word_at(block, index);
		ULONG shift = step_shift[step / WORDS][step % 4];
		a = d;
		d = c;
		c = b;
		b += (mixed << shift) | (mixed >> (32 - shift));
	}
	md5->state[0] += a;
	md5->state[1] += b;
	md5->state[2] += c;
	md5->state[3] += d;
}

/**
 * md5_start() - Begin a digest.
 * @md5: the digest
 */
void md5_start(struct md5 *md5)
{
	static const ULONG initial[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
	memcpy(md5->state, initial, sizeof(initial));
	md5->length_low = 0;
	md5->length_high = 0;
	md5->used = 0;
}

/**
 * md5_add() - Add bytes to a digest.
 * @md5:    the digest
 * @data:   the bytes
 * @length: how many
 */
void md5_add(struct md5 *md5, const UBYTE *data, ULONG length)
{
	ULONG before = md5->length_low;
	md5->length_low += length;
	if (md5->length_low < before)
		md5->length_high++;
	while (length != 0)
	{
		ULONG piece = MD5_BLOCK - md5->used < length ? MD5_BLOCK - md5->used : length;
		memcpy(md5->block + md5->used, data, piece);
		md5->used += piece;
		data += piece;
		length -= piece;
		if (md5->used == MD5_BLOCK)
		{
			block_take(md5, md5->block);
			md5->used = 0;
		}
	}
}

/**
 * md5_end() - End a digest: pad, add the length, write the digest.
 * @md5:    the digest
 * @digest: gets the 16 bytes
 */
void md5_end(struct md5 *md5, UBYTE digest[MD5_BYTES])
{
	ULONG bits_low = md5->length_low << 3;
	ULONG bits_high = (md5->length_high << 3) | (md5->length_low >> 29);
	static const UBYTE pad[MD5_BLOCK] = {PAD_FIRST};
	ULONG padding = md5->used < LENGTH_AT ? LENGTH_AT - md5->used : MD5_BLOCK + LENGTH_AT - md5->used;
	md5_add(md5, pad, padding);
	UBYTE length[8];
	for (ULONG i = 0; i < 4; i++)
	{
		length[i] = (UBYTE)(bits_low >> (8 * i));
		length[4 + i] = (UBYTE)(bits_high >> (8 * i));
	}
	md5_add(md5, length, sizeof(length));
	for (ULONG i = 0; i < MD5_BYTES; i++)
		digest[i] = (UBYTE)(md5->state[i / 4] >> (8 * (i % 4)));
}
