/* SPDX-License-Identifier: MPL-2.0 */
/*
 * display.h - where the engine's pictures go.
 *
 * Two ways (docs/architecture.md section 7), chosen by the place the front names:
 *
 *   a box of a window   the decoder's converter scales to the box and writes
 *                       the screen's pixel format into its own frames;
 *                       Picasso96 copies each into the window, clipped by
 *                       the layers
 *   a whole screen      the screen's bitmap is several pictures tall, each
 *                       picture height a "part" the converter writes by
 *                       itself; showing a picture scrolls the viewport to
 *                       its part, and the 68k touches no pixel
 *
 * The window and the screen are the front's.  The engine draws from its own
 * task, so the front names another place (or none) before it closes them.
 */
#ifndef KINORA_DISPLAY_H
#define KINORA_DISPLAY_H

#include <exec/types.h>
#include <exec/tasks.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/videocodec.h>

#define DISPLAY_PARTS 4u     /* picture heights a full screen's bitmap has at most: one shown, one leaving or cooling, two the decoder's */
#define DISPLAY_EVEN_STEP 2u /* a picture's sizes and top are whole numbers of this, as the VideoCore's converter wants its rows */

/* The place pictures are shown in */
struct display_place
{
	struct Window *window; /* a box of this window; NULL with a screen, or for nowhere */
	LONG left;             /* the box's left edge in the window */
	LONG top;              /* its top edge */
	LONG width;            /* the box's pixels; a screen's visible pixels */
	LONG height;           /* the box's rows; a screen's picture rows, even: its bitmap is a whole number of them and the foot */
	struct Screen *screen; /* the whole of this screen; NULL for a window's box */
	LONG foot;             /* a screen: rows under the picture's that are the front's; it draws them in the screen's first picture height */
};

#define DISPLAY_ASPECT_FILL (-1) /* an aspect's Num: stretch the picture over the whole place */

struct display; /* a place in use; display.c's own */

struct display *display_open(const struct display_place *place, struct Task *task, ULONG signal);
void display_close(struct display *display);
ULONG display_choose_format(const struct display *display, ULONG codec);
BOOL display_mode_usable(ULONG mode);
BOOL display_attach(struct display *display, struct VCSession *session, ULONG codec, const struct VCStreamInfo *stream,
                    const struct VCRational *aspect);
void display_detach(struct display *display);
BOOL display_busy(const struct display *display);
ULONG display_parts(const struct display *display);
void display_show(struct display *display, struct VCFrame *frame);
VCTime display_poll(struct display *display);
void display_clear(struct display *display);
void display_bars_black(struct display *display, const struct VCFrame *frame);
void display_rect_black(const struct VCFrame *frame, const struct VCRect *rect);

#endif /* KINORA_DISPLAY_H */
