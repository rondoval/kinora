/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * branchless_clip.h - FFmpeg's clamps without branches, forced in ahead of every FFmpeg
 * source file (docs/architecture.md, section 3).
 *
 * Emu68 translates a conditional branch along its predicted path (a short backward branch
 * taken, any other not taken) and makes the other path an exit from the translation unit.
 * FFmpeg's av_clip_uint8_c() and its kind compile to "compare, branch forward when in range",
 * so the common case left the unit once a pixel.  These give the same results with masks
 * made of comparisons instead.  libavutil/common.h defines its own only where these are absent,
 * so FFmpeg's source stays as it is.
 */
#ifndef KINORA_FFMPEG_BRANCHLESS_CLIP_H
#define KINORA_FFMPEG_BRANCHLESS_CLIP_H
#ifndef __ASSEMBLER__ /* FFmpeg's C flags reach its assembly files too (libavcodec/m68k/) */

#include <stdint.h>

#define CLIP_UINT8_MOST 255
#define CLIP_INT8_LEAST (-128)
#define CLIP_INT8_MOST 127
#define CLIP_UINT16_MOST 65535
#define CLIP_INT16_LEAST (-32768)
#define CLIP_INT16_MOST 32767

/**
 * branchless_clip() - Hold an integer to [low, high].
 * @value: the integer
 * @low:   the least result
 * @high:  the greatest result
 *
 * Return: @value, @low or @high.
 */
static inline __attribute__((always_inline, const)) int branchless_clip(int value, int low, int high)
{
	int below = -(value < low);
	value = (value & ~below) | (low & below);
	int above = -(value > high);
	return (value & ~above) | (high & above);
}

/**
 * branchless_clip_uint8() - Hold an integer to 0..255.
 * @value: the integer
 *
 * Return: the byte.
 */
static inline __attribute__((always_inline, const)) uint8_t branchless_clip_uint8(int value)
{
	int below = -(value < 0);
	value &= ~below;
	int above = -(value > CLIP_UINT8_MOST);
	return (uint8_t)(value | above);
}

/**
 * branchless_clip_int8() - Hold an integer to -128..127.
 * @value: the integer
 *
 * Return: the byte.
 */
static inline __attribute__((always_inline, const)) int8_t branchless_clip_int8(int value)
{
	return (int8_t)branchless_clip(value, CLIP_INT8_LEAST, CLIP_INT8_MOST);
}

/**
 * branchless_clip_uint16() - Hold an integer to 0..65535.
 * @value: the integer
 *
 * Return: the word.
 */
static inline __attribute__((always_inline, const)) uint16_t branchless_clip_uint16(int value)
{
	return (uint16_t)branchless_clip(value, 0, CLIP_UINT16_MOST);
}

/**
 * branchless_clip_int16() - Hold an integer to -32768..32767.
 * @value: the integer
 *
 * Return: the word.
 */
static inline __attribute__((always_inline, const)) int16_t branchless_clip_int16(int value)
{
	return (int16_t)branchless_clip(value, CLIP_INT16_LEAST, CLIP_INT16_MOST);
}

#define av_clip branchless_clip
#define av_clip_uint8 branchless_clip_uint8
#define av_clip_int8 branchless_clip_int8
#define av_clip_uint16 branchless_clip_uint16
#define av_clip_int16 branchless_clip_int16

#endif
#endif
