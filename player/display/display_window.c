/* SPDX-License-Identifier: MPL-2.0 */
/*
 * display_window.c - pictures into a box of a window: the decoder's
 * converter scales to the box, Picasso96 copies each picture in, clipped by
 * the layers (docs/architecture.md 7.2).
 */
#include <graphics/rastport.h>
#include <proto/graphics.h>
#include <proto/Picasso96.h>
#include <proto/videocodec.h>

#include "display_private.h"

#define WIDTH_STEP 32u /* the converter writes rows of whole groups of this many pixels */
#define WINDOW_FRAMES 4 /* the decoder's own frames asked for: also says "your own again" after a screen's imported parts */
#define BLACK_PEN 1    /* the pen the box is cleared with */

/**
 * window_attach() - Have the decoder scale its pictures into frames of its own, as large as the box.
 * @display: the display, its session, format and shape set
 *
 * A frame holds the whole box, as far as the converter's rows reach: the
 * picture in its middle, in the shape it is to be shown in, and what the box
 * has beside it, where subtitles can go.
 *
 * Return: TRUE, FALSE when the box is too small or the decoder refused.
 */
BOOL window_attach(struct display *display)
{
	const struct display_place *place = &display->place;
	if (place->width < (LONG)WIDTH_STEP || place->height <= 1)
		return FALSE;
	ULONG width = step_down((ULONG)place->width, WIDTH_STEP);
	ULONG height = step_down((ULONG)place->height, DISPLAY_EVEN_STEP);
	struct VCRect picture;
	if (!shape_fit(display, width, height, &picture))
		return FALSE;
	window_clear(display);
	return VC_SetAttrsTags(display->session, VCA_OutputFormat, display->format, VCA_OutputWidth, width, VCA_OutputHeight, height,
	                       VCA_OutputCount, WINDOW_FRAMES, VCA_DestRect, (ULONG)&picture, TAG_DONE) == 0;
}

/**
 * window_show() - Copy a frame into the middle of the box and give it back to the decoder.
 * @display: the display
 * @frame:   the frame; one larger than the box is cut at its right and bottom
 */
void window_show(struct display *display, struct VCFrame *frame)
{
	const struct display_place *place = &display->place;
	LONG width = (LONG)frame->Width < place->width ? (LONG)frame->Width : place->width;
	LONG height = (LONG)frame->Height < place->height ? (LONG)frame->Height : place->height;
	struct RenderInfo source = {
	    .Memory = frame->Data,
	    .BytesPerRow = (WORD)frame->Pitch[0],
	    .RGBFormat = format_to_p96(display->format),
	};
	p96WritePixelArray(&source, 0, 0, place->window->RPort, (UWORD)(place->left + (place->width - width) / 2),
	                   (UWORD)(place->top + (place->height - height) / 2), (UWORD)width, (UWORD)height);
	VC_ReleaseFrame(display->session, frame);
}

/**
 * window_clear() - Fill the box with black.
 * @display: the display
 */
void window_clear(struct display *display)
{
	const struct display_place *place = &display->place;
	if (place->width <= 0 || place->height <= 0)
		return;
	struct RastPort *port = place->window->RPort;
	SetAPen(port, BLACK_PEN);
	RectFill(port, place->left, place->top, place->left + place->width - 1, place->top + place->height - 1);
}
