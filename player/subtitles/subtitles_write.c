/* SPDX-License-Identifier: MPL-2.0 */
/*
 * subtitles_write.c - the stacks' masks written into each picture's frame,
 * and what each stack left in each frame kept.
 *
 * For every picture the masks are written into the frame's own memory
 * before it is shown, nothing read back, since reading video memory is what
 * costs.
 *
 * A frame holds more than the picture where the picture's shape is not the
 * place's: bars beside it, which the display makes black.  A stack that fits
 * the bar under the picture (over it, for the top's) lies there, as plain
 * coverage on black, smooth all round, and is written once into each frame,
 * not once a picture.  One that does not fit stands over bar and picture.
 * What a stack left in a bar is blacked out when the frame comes round again
 * without it: the subtitles keep, for each of the decoder's frames, what
 * each stack last wrote there.
 */
#include <string.h>

#include "subtitles_private.h"
#include "../display/display.h"

#define PERCENT 100
#define GREY_TO_LONG 0x01010101u /* a grey times this: the same byte in all four of a long word */
#define RUN_CHUNK 256u      /* pixels of a run made in Fast RAM before one copy into the frame */

/**
 * pixel_bytes() - The bytes of a pixel in a picture format.
 * @format: VCF_*
 *
 * Return: the bytes, 0 for a format subtitles are not drawn into.
 */
ULONG pixel_bytes(ULONG format)
{
	switch (format)
	{
	case VCF_RGB16:
		return 2;
	case VCF_RGB24:
	case VCF_BGR24:
		return 3;
	case VCF_BGRA32:
	case VCF_RGBA32:
		return 4;
	default:
		return 0;
	}
}

/**
 * pixel_put() - Write a grey into a pixel.
 * @pixel: the pixel, on a long word for 4 bytes
 * @grey:  0 for black to 255 for white
 * @bytes: the pixel's bytes: 2 for R5G6B5 with the low byte first, else a byte a channel
 */
static inline void pixel_put(UBYTE *pixel, UBYTE grey, ULONG bytes)
{
	switch (bytes)
	{
	case 4:
		*(ULONG *)pixel = grey * GREY_TO_LONG;
		return;
	case 3:
		pixel[0] = pixel[1] = pixel[2] = grey;
		return;
	default:
		pixel[0] = (UBYTE)((grey >> 2 << 5 & 0xe0) | grey >> 3);
		pixel[1] = (UBYTE)((grey & 0xf8) | grey >> 5);
		return;
	}
}

/**
 * run_put() - Write a run of greys into a frame as pixels.
 * @to:    the run's first pixel in the frame
 * @greys: its greys, 0 for black to 255 for white
 * @count: how many
 * @bytes: the bytes of a pixel, as pixel_put() takes them
 *
 * The pixels are made in Fast RAM, RUN_CHUNK at a time, and copied into
 * the frame whole: video memory takes one copy far faster than a store a
 * pixel.
 */
static void run_put(UBYTE *to, const UBYTE *greys, ULONG count, ULONG bytes)
{
	ULONG made[RUN_CHUNK]; /* a long word a pixel at most */
	while (count != 0)
	{
		ULONG part = count < RUN_CHUNK ? count : RUN_CHUNK;
		UBYTE *pixel = (UBYTE *)made;
		for (ULONG i = 0; i < part; i++, pixel += bytes)
			pixel_put(pixel, greys[i], bytes);
		memcpy(to, made, part * bytes);
		to += part * bytes;
		greys += part;
		count -= part;
	}
}

/**
 * row_put() - Write one row of a plane into a frame's row.
 * @to:    the frame's pixel under the row's first
 * @greys: the row
 * @width: its pixels
 * @bytes: the bytes of a pixel
 * @whole: write every pixel; else only the runs that are not 0
 */
static void row_put(UBYTE *to, const UBYTE *greys, ULONG width, ULONG bytes, BOOL whole)
{
	if (whole)
	{
		run_put(to, greys, width, bytes);
		return;
	}
	ULONG x = 0;
	while (x < width)
	{
		while (x < width && greys[x] == 0)
			x++;
		ULONG start = x;
		while (x < width && greys[x] != 0)
			x++;
		if (x > start)
			run_put(to + start * bytes, greys + start, x - start, bytes);
	}
}

/**
 * plane_put() - Write a plane into a frame.
 * @plane: the plane: a mask, or coverage
 * @frame: the frame
 * @bytes: the bytes of one of its pixels
 * @left:  where in the frame the plane's left edge goes
 * @top:   and its top; a plane that does not fit there is not written
 * @whole: write every pixel, as the grey its byte says; else only those that are not 0
 *
 * Nothing is read from the frame.
 */
void plane_put(const struct plane *plane, const struct VCFrame *frame, ULONG bytes, ULONG left, ULONG top, BOOL whole)
{
	if (left + plane->width > frame->Width || top + plane->height > frame->Height)
		return;
	for (ULONG row = 0; row < plane->height; row++)
	{
		UBYTE *to = (UBYTE *)frame->Data + frame->Offset[0] + (top + row) * frame->Pitch[0] + left * bytes;
		row_put(to, plane->bytes + row * plane->width, plane->width, bytes, whole);
	}
}

/**
 * beside_black() - Make black what of a rectangle lies over or under the picture.
 * @frame: the frame
 * @rect:  the rectangle
 */
static void beside_black(const struct VCFrame *frame, const struct VCRect *rect)
{
	LONG picture_bottom = frame->Crop.Y + frame->Crop.Height, bottom = rect->Y + rect->Height;
	if (rect->Y < frame->Crop.Y)
	{
		LONG end = bottom < frame->Crop.Y ? bottom : frame->Crop.Y;
		display_rect_black(frame, &(struct VCRect){.X = rect->X, .Y = rect->Y, .Width = rect->Width, .Height = end - rect->Y});
	}
	if (bottom > picture_bottom)
	{
		LONG start = rect->Y > picture_bottom ? rect->Y : picture_bottom;
		display_rect_black(frame, &(struct VCRect){.X = rect->X, .Y = start, .Width = rect->Width, .Height = bottom - start});
	}
}

/**
 * canvas_of() - The record kept of a frame.
 * @subtitles: the subtitles
 * @frame:     the frame; one not seen since the decoder's frames were made has its bars black (display_bars_black())
 *
 * Return: the record; with more frames than records, one that is used anew every time.
 */
static struct canvas *canvas_of(struct subtitles *subtitles, const struct VCFrame *frame)
{
	for (ULONG i = 0; i < subtitles->canvas_count; i++)
		if (subtitles->canvases[i].data == frame->Data)
			return &subtitles->canvases[i];
	struct canvas *canvas = &subtitles->canvases[subtitles->canvas_count < CANVASES ? subtitles->canvas_count++ : CANVASES - 1];
	*canvas = (struct canvas){.data = frame->Data};
	return canvas;
}

/**
 * stack_place() - Where a stack goes in a frame.
 * @stack:    the stack, drawn
 * @edge:     its edge
 * @distance: the gap from the frame's edge, in percent of its rows; halved where there is a bar
 * @frame:    the frame
 * @place:    gets the rectangle
 *
 * At the frame's foot, or at its top, the gap kept: in the bar beside the
 * picture when the stack fits there, else over bar and picture.  The gap
 * does not follow the stack's height, so a cue stays put while others come
 * and go.  A stack is at most half the frame's rows and the gap under a
 * third, so it always fits the frame.
 *
 * Return: TRUE when all of it lies in the bar.
 */
static BOOL stack_place(const struct stack *stack, enum edge edge, ULONG distance, const struct VCFrame *frame, struct VCRect *place)
{
	LONG height = (LONG)stack->mask.height, rows = (LONG)frame->Height;
	LONG bar = edge == EDGE_TOP ? frame->Crop.Y : rows - (frame->Crop.Y + frame->Crop.Height);
	LONG gap = rows * (LONG)distance / PERCENT / (bar > 0 ? 2 : 1);
	*place = (struct VCRect){
	    .X = frame->Crop.X + (frame->Crop.Width - (LONG)stack->mask.width) / 2,
	    .Y = edge == EDGE_TOP || rows < gap + height ? gap : rows - gap - height,
	    .Width = (LONG)stack->mask.width,
	    .Height = height,
	};
	return bar >= gap + height;
}

/**
 * stack_write() - Write a stack into a frame, and take away what it left there before beside the picture.
 * @subtitles: the subtitles
 * @canvas:    the frame's record
 * @edge:      the stack's edge
 * @frame:     the frame
 * @bytes:     the bytes of one of its pixels
 */
static void stack_write(const struct subtitles *subtitles, struct canvas *canvas, enum edge edge, const struct VCFrame *frame, ULONG bytes)
{
	const struct stack *stack = &subtitles->stacks[edge];
	struct written *written = &canvas->written[edge];
	struct VCRect place = {0};
	BOOL drawn = stack->text_plane.bytes != NULL;
	BOOL beside = drawn && stack_place(stack, edge, subtitles->distance, frame, &place);
	if (written->version == stack->version && memcmp(&written->rect, &place, sizeof(place)) == 0 && (!drawn || written->beside))
		return; /* nothing to change: what the frame holds beside its picture is right */
	if (written->rect.Height != 0)
		beside_black(frame, &written->rect);
	*written = (struct written){.version = stack->version, .rect = place, .beside = beside};
	if (drawn)
		plane_put(beside ? &stack->text_plane : &stack->mask, frame, bytes, (ULONG)place.X, (ULONG)place.Y, beside);
}

/**
 * subtitles_draw() - Write the cues of a picture's time into the picture's frame.
 * @subtitles: the subtitles; NULL does nothing
 * @time:      the picture's time, VC_TIME_UNKNOWN to show no cue
 * @frame:     the frame, about to be shown
 * @format:    VCF_* of its pixels
 */
void subtitles_draw(struct subtitles *subtitles, VCTime time, const struct VCFrame *frame, ULONG format)
{
	ULONG bytes = pixel_bytes(format);
	if (subtitles == NULL || bytes == 0)
		return;
	struct canvas *canvas = canvas_of(subtitles, frame);
	const struct cue *shown[SHOWN_MOST];
	ULONG count = time != VC_TIME_UNKNOWN ? cues_at(subtitles, time, shown, SHOWN_MOST) : 0;
	for (enum edge edge = EDGE_BOTTOM; edge < EDGES; edge++)
	{
		stack_update(subtitles, edge, shown, count, frame);
		stack_write(subtitles, canvas, edge, frame, bytes);
	}
}
