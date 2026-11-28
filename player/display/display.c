/* SPDX-License-Identifier: MPL-2.0 */
/*
 * display.c - a place for pictures: opened and closed, the pixel format to
 * decode to, the bars beside the picture made black, and each call sent to
 * the window's or the screen's way of showing (display.h).
 */
#include <string.h>
#include <exec/memory.h>
#include <graphics/modeid.h>
#include <proto/exec.h>
#include <proto/Picasso96.h>
#include <proto/videocodec.h>

#include "display_private.h"

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define START_STEP 4u /* the picture's left edge: on a whole long word of the frame in every pixel format, as VCA_DestRect wants */

/* A pixel format the converter writes and Picasso96 shows */
struct pixel_format
{
	RGBFTYPE p96; /* Picasso96's name */
	ULONG format; /* VCF_* */
	ULONG bytes;  /* of a pixel; all bytes 0 is black in each */
};

static const struct pixel_format pixel_formats[] = {
    {RGBFB_R5G6B5PC, VCF_RGB16, 2}, {RGBFB_B8G8R8A8, VCF_BGRA32, 4}, {RGBFB_R8G8B8A8, VCF_RGBA32, 4},
    {RGBFB_R8G8B8, VCF_RGB24, 3},   {RGBFB_B8G8R8, VCF_BGR24, 3},
};

/**
 * format_from_p96() - The picture format that is a Picasso96 pixel format.
 * @p96: the RGBFTYPE
 *
 * Return: VCF_*, VCF_Unknown for one the converter does not write.
 */
static ULONG format_from_p96(RGBFTYPE p96)
{
	for (ULONG i = 0; i < ARRAY_COUNT(pixel_formats); i++)
		if (pixel_formats[i].p96 == p96)
			return pixel_formats[i].format;
	return VCF_Unknown;
}

/**
 * format_to_p96() - Picasso96's name for a picture format.
 * @format: VCF_*
 *
 * Return: the RGBFTYPE, RGBFB_NONE for one Picasso96 does not show.
 */
RGBFTYPE format_to_p96(ULONG format)
{
	for (ULONG i = 0; i < ARRAY_COUNT(pixel_formats); i++)
		if (pixel_formats[i].format == format)
			return pixel_formats[i].p96;
	return RGBFB_NONE;
}

/**
 * display_mode_usable() - Whether the full screen can be in a screen mode.
 * @mode: the mode's display ID
 *
 * Return: TRUE for a Picasso96 mode whose pixel format the converter writes;
 * FALSE for any other, and for one that does not exist (any more).
 */
BOOL display_mode_usable(ULONG mode)
{
	return mode != (ULONG)INVALID_ID && p96GetModeIDAttr(mode, P96IDA_ISP96) &&
	       format_from_p96((RGBFTYPE)p96GetModeIDAttr(mode, P96IDA_RGBFORMAT)) != VCF_Unknown;
}

/**
 * format_pick() - The best of pixel_formats[] that a mask holds, for a screen.
 * @mask:   VCF_MASK()s of the formats a session can write
 * @wanted: the screen's own format as VCF_*
 * @exact:  only the screen's own will do
 *
 * Return: the screen's own when the mask holds it; else, unless @exact, the first the mask holds; else VCF_Unknown.
 */
static ULONG format_pick(ULONG mask, ULONG wanted, BOOL exact)
{
	if (wanted != VCF_Unknown && (mask & VCF_MASK(wanted)) != 0)
		return wanted;
	for (ULONG i = 0; !exact && i < ARRAY_COUNT(pixel_formats); i++)
		if ((mask & VCF_MASK(pixel_formats[i].format)) != 0)
			return pixel_formats[i].format;
	return VCF_Unknown;
}

/**
 * format_bytes() - The bytes of a pixel in a picture format.
 * @format: VCF_*
 *
 * Return: the bytes, 0 for a format not in pixel_formats[].
 */
static ULONG format_bytes(ULONG format)
{
	for (ULONG i = 0; i < ARRAY_COUNT(pixel_formats); i++)
		if (pixel_formats[i].format == format)
			return pixel_formats[i].bytes;
	return 0;
}

/**
 * display_choose_format() - Choose the pixel format to ask a decoder session for.
 * @display: the place
 * @codec:   VCC_*: the stream's codec
 *
 * The bitmap's own format when the decoder or the converter behind it writes
 * it.  A window's box also takes another, which Picasso96 converts as it
 * copies: one the decoder writes itself before one the converter adds.  A
 * screen's bitmap takes its own only.
 *
 * Return: VCF_*, VCF_Unknown when no backend decodes the codec to a format that can be shown there.
 */
ULONG display_choose_format(const struct display *display, ULONG codec)
{
	BOOL exact = display->place.screen != NULL;
	struct BitMap *bitmap = exact ? display->place.screen->RastPort.BitMap : display->place.window->RPort->BitMap;
	ULONG wanted = format_from_p96((RGBFTYPE)p96GetBitMapAttr(bitmap, P96BMA_RGBFORMAT));
	struct VCCodecInfo row = {.Size = sizeof(row)};
	for (ULONG index = 0; VC_QueryCodec(index, &row) == 0; index++, row.Size = sizeof(row))
	{
		if (row.Codec != codec || row.Kind != VCK_Decoder || (row.Flags & VCCF_Session) == 0)
			continue;
		ULONG chained = VC_HAS_FIELD(row.Size, struct VCCodecInfo, ChainOutputFormats) ? row.ChainOutputFormats : 0;
		ULONG any = format_pick(row.OutputFormats | chained, wanted, exact);
		ULONG own = format_pick(row.OutputFormats, wanted, exact);
		if (any == wanted || own == VCF_Unknown)
			own = any;
		if (own != VCF_Unknown)
			return own;
	}
	return VCF_Unknown;
}

/**
 * display_open() - Take a place into use.
 * @place:  the place; NULL, or one with neither a window nor a screen, is nowhere
 * @task:   the engine's task
 * @signal: what to signal it with when a picture has reached the screen
 *
 * Return: the display, NULL for nowhere, without memory, or for a screen whose bitmap the decoder cannot write or
 * whose flipper did not start.
 */
struct display *display_open(const struct display_place *place, struct Task *task, ULONG signal)
{
	if (place == NULL || (place->window == NULL && place->screen == NULL))
		return NULL;
	struct display *display = AllocVec(sizeof(*display), MEMF_PUBLIC | MEMF_CLEAR);
	if (display == NULL)
		return NULL;
	display->place = *place;
	display->task = task;
	display->signal = signal;
	display->part_shown = 0;
	display->part_leaving = NO_PART;
	display->part_cooling = NO_PART;
	display->flip_to = NO_PART;
	if (place->screen != NULL && !screen_open(display))
	{
		display_close(display);
		return NULL;
	}
	return display;
}

/**
 * display_close() - Give a place up.
 * @display: the display, no decoder attached; NULL does nothing
 */
void display_close(struct display *display)
{
	if (display == NULL)
		return;
	if (display->place.screen != NULL)
		screen_close(display);
	FreeVec(display->foot_kept);
	FreeVec(display->foot_fresh);
	FreeVec(display);
}

/**
 * shape_set() - Note the shape the picture is to be shown in: its width to its height.
 * @display: the display
 * @stream:  the stream: its visible picture and its pixel aspect
 * @aspect:  the shape picked in the View menu (16:9, 4:3, ...), Num 0 for the film's own, or Num DISPLAY_ASPECT_FILL
 *
 * A shape is a ratio, not a size: shape_fit() makes the largest picture of
 * it that a place holds.  The film's own is its visible picture with its
 * pixels' aspect counted: 1440 by 1080 pixels of 4:3 are 5760:3240, 16:9.
 */
static void shape_set(struct display *display, const struct VCStreamInfo *stream, const struct VCRational *aspect)
{
	if (aspect->Num == DISPLAY_ASPECT_FILL)
	{
		display->shape = (struct VCRational){0, 0}; /* none: stretched over the whole place */
		return;
	}
	if (aspect->Num > 0 && aspect->Den > 0)
	{
		display->shape = *aspect; /* the one picked, in place of the film's */
		return;
	}
	BOOL has_pixel_aspect = stream->Par.Num > 0 && stream->Par.Den > 0; /* a stream that names none has square pixels */
	display->shape.Num = stream->Crop.Width * (has_pixel_aspect ? stream->Par.Num : 1);
	display->shape.Den = stream->Crop.Height * (has_pixel_aspect ? stream->Par.Den : 1);
}

/**
 * scaled() - A size times a ratio.
 * @size:  the size
 * @times: the ratio's top
 * @over:  its bottom, not 0
 *
 * The product is taken in 64 bits: a shape is a picture's size times its
 * pixels' aspect, and both can be large.
 *
 * Return: @size * @times / @over, rounded down.
 */
static ULONG scaled(ULONG size, ULONG times, ULONG over)
{
	return (ULONG)((unsigned long long)size * times / over);
}

/**
 * shape_fit() - The largest picture of the shape to show that fits a box, and where it stands in it.
 * @display:    the display, its shape set
 * @box_width:  the box's pixels
 * @box_height: the box's rows
 * @picture:    gets the picture, centred in the box, its sizes even, its left edge a whole number of START_STEP pixels
 *
 * The picture takes the box's whole width, unless its shape then makes it
 * taller than the box: then it takes the box's whole height, and its width
 * follows from that.  Without a shape (stretched to fill) it is the whole box.
 *
 * Return: TRUE, FALSE for a box too small for any picture.
 */
BOOL shape_fit(const struct display *display, ULONG box_width, ULONG box_height, struct VCRect *picture)
{
	ULONG width = step_down(box_width, DISPLAY_EVEN_STEP);
	ULONG height = step_down(box_height, DISPLAY_EVEN_STEP);
	if (display->shape.Num > 0 && display->shape.Den > 0)
	{
		/* as wide as the box, and as tall as the shape makes that */
		height = step_down(scaled(width, (ULONG)display->shape.Den, (ULONG)display->shape.Num), DISPLAY_EVEN_STEP);
		if (height > box_height)
		{
			/* too tall: as tall as the box, and as wide as the shape makes that */
			height = step_down(box_height, DISPLAY_EVEN_STEP);
			width = step_down(scaled(height, (ULONG)display->shape.Num, (ULONG)display->shape.Den), DISPLAY_EVEN_STEP);
		}
	}
	/* centred: half of what is left over on each side, the left edge moved left to where the decoder can start a row */
	*picture = (struct VCRect){
	    .X = (LONG)step_down((box_width - width) / 2, START_STEP),
	    .Y = (LONG)step_down((box_height - height) / 2, DISPLAY_EVEN_STEP),
	    .Width = (LONG)width,
	    .Height = (LONG)height,
	};
	return width != 0 && height != 0;
}

/**
 * display_attach() - Have a decoder make its pictures for the place.
 * @display: the display; NULL does nothing
 * @session: the decoder
 * @codec:   VCC_*: its stream's codec
 * @stream:  its stream's shape
 * @aspect:  the shape to show the picture in, width to height; Num 0 for the stream's own, DISPLAY_ASPECT_FILL to
 *           stretch it over the whole place
 *
 * The decoder's output frames are replaced: pictures it gave before are gone.
 *
 * Return: TRUE, FALSE when there is no display, the place is too small or the decoder refused.
 */
BOOL display_attach(struct display *display, struct VCSession *session, ULONG codec, const struct VCStreamInfo *stream,
                    const struct VCRational *aspect)
{
	if (display == NULL)
		return FALSE;
	display_detach(display);
	display->session = session;
	display->bars_black_count = 0; /* the decoder's frames are made anew */
	display->format = display_choose_format(display, codec);
	shape_set(display, stream, aspect);
	BOOL attached = display->format != VCF_Unknown && (display->place.screen != NULL ? screen_attach(display) : window_attach(display));
	if (!attached)
		display->session = NULL;
	return attached;
}

/**
 * display_detach() - Forget the decoder: its frames are gone, or about to be.
 * @display: the display; NULL does nothing
 */
void display_detach(struct display *display)
{
	if (display == NULL)
		return;
	if (display->place.screen != NULL)
		screen_detach(display);
	display->session = NULL;
}

/**
 * display_busy() - Whether the display cannot take a picture just now.
 * @display: the display; NULL is never busy
 *
 * Return: TRUE while the last picture is still on its way to the screen.
 */
BOOL display_busy(const struct display *display)
{
	return display != NULL && display->flip_to != NO_PART;
}

/**
 * display_parts() - The picture heights a screen's bitmap has.
 * @display: the display; NULL has none
 *
 * Return: the parts; 0 for a window's box.
 */
ULONG display_parts(const struct display *display)
{
	return display != NULL && display->place.screen != NULL ? display->part_count : 0;
}

/**
 * display_show() - Show a picture.
 * @display: the display, a decoder attached and not busy
 * @frame:   the picture; the display's from here on, and the decoder's again when it can be written
 */
void display_show(struct display *display, struct VCFrame *frame)
{
	if (display->place.screen != NULL)
		screen_show(display, frame);
	else
		window_show(display, frame);
}

/**
 * display_poll() - Do what the display has to do between pictures.
 * @display: the display; NULL does nothing
 *
 * Return: the microseconds after which it wants to be called again, 0 for no need.
 */
VCTime display_poll(struct display *display)
{
	return display != NULL && display->place.screen != NULL ? screen_poll(display) : 0;
}

/**
 * display_clear() - Make the place black.
 * @display: the display, no decoder attached; NULL does nothing
 */
void display_clear(struct display *display)
{
	if (display == NULL)
		return;
	if (display->place.screen != NULL)
		screen_clear(display);
	else
		window_clear(display);
}

/**
 * display_rect_black() - Make a rectangle of a frame black.
 * @frame: the frame
 * @rect:  the pixels; what lies outside the frame is left out
 */
void display_rect_black(const struct VCFrame *frame, const struct VCRect *rect)
{
	ULONG bytes = format_bytes(frame->Format);
	LONG left = rect->X > 0 ? rect->X : 0;
	LONG top = rect->Y > 0 ? rect->Y : 0;
	LONG right = rect->X + rect->Width < (LONG)frame->Width ? rect->X + rect->Width : (LONG)frame->Width;
	LONG bottom = rect->Y + rect->Height < (LONG)frame->Height ? rect->Y + rect->Height : (LONG)frame->Height;
	if (bytes == 0 || right <= left)
		return;
	UBYTE *first = (UBYTE *)frame->Data + frame->Offset[0] + (ULONG)left * bytes; /* the left edge's pixel in the frame's first row */
	for (LONG row = top; row < bottom; row++)
		memset(first + (ULONG)row * frame->Pitch[0], 0, (ULONG)(right - left) * bytes);
}

/**
 * bars_make_black() - Make black everything of a frame beside its picture.
 * @frame: the frame
 */
static void bars_make_black(const struct VCFrame *frame)
{
	const struct VCRect *picture = &frame->Crop;
	LONG width = (LONG)frame->Width;
	LONG height = (LONG)frame->Height;
	LONG right = picture->X + picture->Width;
	display_rect_black(frame, &(struct VCRect){.X = 0, .Y = 0, .Width = width, .Height = picture->Y});
	display_rect_black(frame, &(struct VCRect){.X = 0, .Y = picture->Y + picture->Height, .Width = width, .Height = height});
	display_rect_black(frame, &(struct VCRect){.X = 0, .Y = picture->Y, .Width = picture->X, .Height = picture->Height});
	display_rect_black(frame, &(struct VCRect){.X = right, .Y = picture->Y, .Width = width - right, .Height = picture->Height});
}

/**
 * display_bars_black() - Make a frame's bars black, the first time it comes after the decoder's frames were made.
 * @display: the display; NULL does nothing
 * @frame:   the picture, before anything is drawn over it
 *
 * The decoder writes the picture only and leaves beside it what the memory
 * held.  A frame beyond those the display keeps count of is made black
 * every time it comes.
 */
void display_bars_black(struct display *display, const struct VCFrame *frame)
{
	if (display == NULL)
		return;
	for (ULONG i = 0; i < display->bars_black_count; i++)
		if (display->bars_black[i] == frame->Data)
			return;
	if (display->bars_black_count < BARS_KNOWN)
		display->bars_black[display->bars_black_count++] = frame->Data;
	bars_make_black(frame);
}
