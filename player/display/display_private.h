/* SPDX-License-Identifier: MPL-2.0 */
/*
 * display_private.h - the display's own structures.
 *
 *   display.c          a place opened and closed, pixel formats, the bars made black, and each call sent to the way the place asks for
 *   display_window.c   pictures copied into a box of a window
 *   display_screen.c   pictures written into the parts of a screen's bitmap and shown by scrolling
 */
#ifndef KINORA_DISPLAY_PRIVATE_H
#define KINORA_DISPLAY_PRIVATE_H

#include <dos/dosextens.h>
#include <libraries/Picasso96.h>

#include "display.h"

#define NO_PART (-1L) /* part_shown, part_leaving, part_cooling, flip_to: no part */
#define BARS_KNOWN 8u  /* frames the display keeps count of as having black bars: a window's four, or a screen's parts */

/**
 * step_down() - A number down to a whole number of steps.
 * @value: the number
 * @step:  the step
 *
 * Return: the largest multiple of @step that is not above @value.
 */
static inline ULONG step_down(ULONG value, ULONG step)
{
	return value / step * step;
}

/* A place in use */
struct display
{
	/* the place, and the pictures shown in it */
	struct display_place place; /* the place */
	struct VCSession *session;  /* the decoder whose pictures are shown, NULL while none is attached */
	ULONG format;               /* VCF_* its pictures come in */
	struct VCRational shape;    /* the picture's shape as it is to be shown, a ratio: its width to its height; 0:0 to stretch it */

	/* the frames whose bars are black: the decoder never writes beside the picture */
	APTR bars_black[BARS_KNOWN]; /* their buffers, since the decoder's frames were last made */
	ULONG bars_black_count;      /* how many */

	/* a screen: its bitmap, and the picture heights in it */
	struct RenderInfo bitmap;                   /* where the screen's bitmap is */
	ULONG part_count;                           /* the picture heights it has */
	struct VCFrame *part_frames[DISPLAY_PARTS]; /* each part as the decoder's output frame, NULL while none is attached */

	/* where each part is: one on display, one leaving it, one cooling; the others are the decoder's */
	LONG part_shown;   /* the part on display */
	LONG part_leaving; /* the part the flip under way takes off the display */
	LONG part_cooling; /* a part off the display that the display may still be reading */
	VCTime cool_until; /* the E-clock time from which it is not */

	/* the flipper, the process that scrolls the viewport, and what it and the engine share */
	struct Process *flipper;      /* the process, NULL to scroll in the engine's task */
	struct Task *task;            /* the engine's task, which it tells */
	ULONG signal;                 /* the signal bits it tells the engine with: a flip done, the flipper gone */
	volatile LONG flip_to;        /* the part the flipper is to show, NO_PART when it has */
	volatile VCTime flip_done_at; /* the E-clock time its last flip was done */
	volatile BOOL flipper_gone;   /* the flipper has ended */

	/* the front's rows under the picture, its controls: read from the first part, carried to the others */
	UBYTE *foot_kept;    /* the rows as last read, NULL without any */
	UBYTE *foot_fresh;   /* room to read them into again */
	VCTime foot_read_at; /* the E-clock time they were last read */
};

RGBFTYPE format_to_p96(ULONG format);

BOOL shape_fit(const struct display *display, ULONG box_width, ULONG box_height, struct VCRect *picture);

BOOL window_attach(struct display *display);
void window_show(struct display *display, struct VCFrame *frame);
void window_clear(struct display *display);

BOOL screen_open(struct display *display);
void screen_close(struct display *display);
BOOL screen_attach(struct display *display);
void screen_detach(struct display *display);
void screen_show(struct display *display, struct VCFrame *frame);
VCTime screen_poll(struct display *display);
void screen_clear(struct display *display);

#endif /* KINORA_DISPLAY_PRIVATE_H */
