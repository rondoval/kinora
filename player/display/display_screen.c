/* SPDX-License-Identifier: MPL-2.0 */
/*
 * display_screen.c - pictures on a whole screen: the screen's bitmap is
 * several picture heights tall, each height a part the decoder's converter
 * writes by itself, and a picture is shown by scrolling the viewport to its
 * part (docs/architecture.md 7.1).
 *
 * Of the parts one is on display; the others are the decoder's to write.  A
 * part taken off the display stays ours for one more refresh, since the
 * display may still be reading it, and goes back to the decoder then.
 *
 * Picasso96 waits for the vertical blank inside ScrollVPort(), and
 * VideoCore.card does so by polling for the one line where the blank begins:
 * a poller that is not running while that line passes waits a whole refresh
 * more.  So the scroll runs in a process of its own, one priority above the
 * engine: the engine never waits in it, and does not hold it up.
 */
#include <dos/dos.h>
#include <dos/dostags.h>
#include <string.h>
#include <exec/memory.h>
#include <graphics/view.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/Picasso96.h>
#include <proto/videocodec.h>

#include "display_private.h"
#include "../engine/clock.h"
#include "../engine/trace.h"

#define COOL_TIME 25000        /* microseconds a part off the display may still be read: one refresh at 50 Hz, with margin */
#define FLIPPER_STACK 8192
#define PITCH_PIXELS 32u       /* the converter writes rows of whole groups of this many pixels */
#define BYTES_ALIGNED 3u       /* a part's address has these bits clear */
#define FOOT_TIME 200000       /* microseconds after which the front's rows are read again, and carried to the other parts if changed */

static struct display *flipper_display; /* the display the flipper being started works for */

/**
 * part_rows() - The rows from one part's first to the next part's: a picture height and the front's rows under it.
 * @display: the display
 *
 * Return: the rows.
 */
static ULONG part_rows(const struct display *display)
{
	return (ULONG)(display->place.height + display->place.foot);
}

/**
 * foot_bytes() - The bytes of the front's rows.
 * @display: the display
 *
 * Return: the bytes, 0 without such rows.
 */
static ULONG foot_bytes(const struct display *display)
{
	return display->place.foot > 0 ? (ULONG)display->place.foot * (ULONG)display->bitmap.BytesPerRow : 0;
}

/**
 * foot_carry() - Write the front's rows, as last read, into every part but the first.
 * @display: the display
 *
 * The front draws its controls in the first part only, which is where
 * Intuition has its window; every part shown must carry them.  The decoder
 * leaves those rows alone, so a part keeps them until they change.
 */
static void foot_carry(struct display *display)
{
	ULONG pitch = (ULONG)display->bitmap.BytesPerRow;
	UBYTE *first = (UBYTE *)display->bitmap.Memory + (ULONG)display->place.height * pitch;
	for (ULONG part = 1; part < display->part_count; part++)
		CopyMem(display->foot_kept, first + part * part_rows(display) * pitch, foot_bytes(display));
}

/**
 * foot_refresh() - Read the front's rows from the first part again, if they are due, and carry them to the other parts if they changed.
 * @display: the display
 *
 * Nothing tells the display when the controls change: ReAction redraws
 * them in the first part by itself (the slider, the time, a pressed
 * button), so the rows it left there are read to see.  Reading and writing
 * video memory is what costs, so the rows are read FOOT_TIME apart and
 * written only when they are not what was read last.
 */
static void foot_refresh(struct display *display)
{
	ULONG bytes = foot_bytes(display);
	VCTime now = clock_wall();
	if (display->foot_fresh == NULL || (display->foot_kept != NULL && now - display->foot_read_at < FOOT_TIME))
		return;

	display->foot_read_at = now;
	CopyMem((UBYTE *)display->bitmap.Memory + (ULONG)display->place.height * (ULONG)display->bitmap.BytesPerRow, display->foot_fresh, bytes);
	if (display->foot_kept != NULL && memcmp(display->foot_kept, display->foot_fresh, bytes) == 0)
		return;

	UBYTE *kept = display->foot_kept != NULL ? display->foot_kept : AllocVec(bytes, MEMF_ANY);
	display->foot_kept = display->foot_fresh;
	display->foot_fresh = kept;
	foot_carry(display);
}

/**
 * part_scroll_to() - Scroll the viewport to a part.
 * @display: the display
 * @part:    the part
 */
static void part_scroll_to(struct display *display, LONG part)
{
	struct ViewPort *port = &display->place.screen->ViewPort;
	port->RasInfo->RyOffset = (WORD)((ULONG)part * part_rows(display));
	TRACE_T0(asked);
	ScrollVPort(port);
	TRACE_ADD(TRACE_FLIP, asked);
	display->flip_done_at = clock_wall();
}

/**
 * flipper_entry() - The flipper process: scroll to the part asked for, tell the engine, until told to end.
 *
 * Ctrl-F asks for display->flip_to; Ctrl-C ends it.
 */
static void flipper_entry(void)
{
	struct display *display = flipper_display;
	for (;;)
	{
		ULONG signals = Wait(SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C);
		if ((signals & SIGBREAKF_CTRL_F) != 0 && display->flip_to != NO_PART)
		{
			part_scroll_to(display, display->flip_to);
			display->flip_to = NO_PART;
			Signal(display->task, display->signal);
		}
		if ((signals & SIGBREAKF_CTRL_C) != 0)
			break;
	}
	Forbid(); /* until the process is gone: the engine frees the display */
	display->flipper_gone = TRUE;
	Signal(display->task, display->signal);
}

/**
 * flip_wait() - Wait until the flip under way is done.
 * @display: the display
 */
static void flip_wait(struct display *display)
{
	while (display->flip_to != NO_PART)
		Wait(display->signal);
	Signal(display->task, display->signal); /* the engine's loop looks at what else the signal meant */
}

/**
 * screen_open() - Find the screen's bitmap and start the flipper.
 * @display: the display, its place set
 *
 * The bitmap must be in the card's memory, its rows whole groups of 32
 * pixels, and at least one of the place's heights tall.  A flipper that
 * does not start refuses the screen: the engine scrolling for itself waits
 * in ScrollVPort() and drops pictures (docs/architecture.md 7.1).
 *
 * Return: TRUE, FALSE for a bitmap the decoder cannot write or no flipper;
 * display_close() then frees what was made.
 */
BOOL screen_open(struct display *display)
{
	struct BitMap *bitmap = display->place.screen->RastPort.BitMap;
	LONG lock = p96LockBitMap(bitmap, (UBYTE *)&display->bitmap, sizeof(display->bitmap));
	if (lock == 0)
		return FALSE;
	p96UnlockBitMap(bitmap, lock); /* a shown bitmap stays where it is */

	ULONG pixel_bytes = p96GetBitMapAttr(bitmap, P96BMA_BYTESPERPIXEL);
	BOOL in_card = p96GetBitMapAttr(bitmap, P96BMA_ISONBOARD) != 0;
	BOOL aligned = pixel_bytes != 0 && ((ULONG)display->bitmap.BytesPerRow / pixel_bytes) % PITCH_PIXELS == 0 &&
	               ((ULONG)display->bitmap.Memory & BYTES_ALIGNED) == 0;
	if (!in_card || !aligned || display->place.height <= 0)
		return FALSE;
	display->part_count = (ULONG)display->place.screen->Height / part_rows(display);
	if (display->part_count > DISPLAY_PARTS)
		display->part_count = DISPLAY_PARTS;
	if (display->part_count == 0)
		return FALSE;
	if (foot_bytes(display) != 0)
		display->foot_fresh = AllocVec(foot_bytes(display), MEMF_ANY); /* without it the front's rows show in the first part only */

	flipper_display = display;
	display->flipper = CreateNewProcTags(NP_Entry, (ULONG)flipper_entry,
	                                     NP_Name, (ULONG) "Kinora flipper",
	                                     NP_Priority, (ULONG)(LONG)(display->task->tc_Node.ln_Pri + 1),
	                                     NP_StackSize, FLIPPER_STACK,
	                                     TAG_DONE);
	return display->flipper != NULL;
}

/**
 * screen_close() - End the flipper.
 * @display: the display, no decoder attached
 */
void screen_close(struct display *display)
{
	if (display->flipper == NULL)
		return;
	Signal(&display->flipper->pr_Task, SIGBREAKF_CTRL_C);
	while (!display->flipper_gone)
		Wait(display->signal);
	display->flipper = NULL;
	Signal(display->task, display->signal);
}

/**
 * screen_attach() - Have the decoder write its pictures into the bitmap's parts.
 * @display: the display, its session and format set
 *
 * The picture's place in a part is worked out here, centred, in the shape
 * it is to be shown in.  Each part is imported as one of the decoder's
 * output frames and knows its number.  The part on display stays ours; the
 * others go to the decoder to be written.  With one part only there is
 * nothing to scroll between: it is shown while it is written.
 *
 * Return: TRUE, FALSE when the decoder refused the bitmap.
 */
BOOL screen_attach(struct display *display)
{
	ULONG pitch = (ULONG)display->bitmap.BytesPerRow;
	struct VCRect picture;
	if (display->place.width <= 0 || display->place.height <= 0 ||
	    !shape_fit(display, (ULONG)display->place.width, (ULONG)display->place.height, &picture))
		return FALSE;
	if (VC_SetAttrsTags(display->session, VCA_OutputFormat, display->format, VCA_OutputWidth, (ULONG)display->place.width,
	                    VCA_OutputHeight, (ULONG)display->place.height, VCA_OutputPitch, pitch, VCA_DestRect, (ULONG)&picture,
	                    TAG_DONE) != 0)
		return FALSE;

	for (ULONG part = 0; part < display->part_count; part++)
	{
		UBYTE *memory = (UBYTE *)display->bitmap.Memory + part * pitch * part_rows(display);
		display->part_frames[part] = VC_ImportFrame(display->session, VCS_Output, memory);
		if (display->part_frames[part] == NULL)
			return FALSE;
		display->part_frames[part]->UserData = (APTR)part;
	}
	/* the decoder gets every part but the one on display, which it would otherwise write while it is seen; with one
	   part only it gets that one too, or it would have none to write */
	BOOL keep_shown = display->part_count > 1;
	for (ULONG part = 0; part < display->part_count; part++)
		if (!keep_shown || (LONG)part != display->part_shown)
			VC_ReleaseFrame(display->session, display->part_frames[part]);
	return TRUE;
}

/**
 * part_give_back() - Give a part that is off the display back to the decoder.
 * @display: the display
 * @part:    the part, NO_PART for none
 */
static void part_give_back(struct display *display, LONG part)
{
	if (part != NO_PART && display->part_frames[part] != NULL)
		VC_ReleaseFrame(display->session, display->part_frames[part]);
}

/**
 * screen_detach() - Forget the decoder's frames over the parts.
 * @display: the display
 *
 * A flip under way is waited for; the part on display stays on display, though the decoder may write it again.
 */
void screen_detach(struct display *display)
{
	flip_wait(display);
	if (display->session != NULL && display->part_count > 1)
	{
		/* the parts kept from the decoder go back to it: it does not make new frames while old ones are out */
		part_give_back(display, display->part_shown);
		part_give_back(display, display->part_leaving);
		part_give_back(display, display->part_cooling);
	}
	for (ULONG part = 0; part < DISPLAY_PARTS; part++)
		display->part_frames[part] = NULL;
	display->part_leaving = NO_PART;
	display->part_cooling = NO_PART;
}

/**
 * flip_settle() - A flip is done: the part it took off the display cools from then on.
 * @display: the display
 *
 * A part still cooling has by then been off the display for a refresh,
 * since every flip waits for the blank, and goes back to the decoder.
 */
static void flip_settle(struct display *display)
{
	if (display->part_leaving == NO_PART || display->flip_to != NO_PART)
		return;
	part_give_back(display, display->part_cooling);
	display->part_cooling = display->part_leaving;
	display->cool_until = display->flip_done_at + COOL_TIME;
	display->part_leaving = NO_PART;
}

/**
 * screen_show() - Show the picture in a part.
 * @display: the display, no flip under way
 * @frame:   the picture: one of the parts
 *
 * The part on display leaves it.  The flipper is asked for the scroll, and
 * the display is busy until it is done.
 */
void screen_show(struct display *display, struct VCFrame *frame)
{
	LONG part = (LONG)frame->UserData;
	if (display->part_count == 1)
	{
		VC_ReleaseFrame(display->session, frame);
		return;
	}
	flip_settle(display); /* the flip before this one, if nothing looked since it was done */
	foot_refresh(display);
	display->part_leaving = display->part_shown;
	display->part_shown = part;
	display->flip_to = part;
	Signal(&display->flipper->pr_Task, SIGBREAKF_CTRL_F);
}

/**
 * screen_poll() - Follow the parts off the display: one just hidden cools, one cooled goes back to the decoder.
 * @display: the display
 *
 * While the front has rows under the picture, they are looked at here, and carried to the other parts when they changed.
 *
 * Return: the microseconds after which to call again: until the cooling part is cooled or the rows are due; 0 for no need.
 */
VCTime screen_poll(struct display *display)
{
	if (display->session == NULL)
		return 0;
	flip_settle(display);
	VCTime again = 0;
	if (display->place.foot > 0)
	{
		foot_refresh(display);
		again = FOOT_TIME;
	}
	if (display->part_cooling == NO_PART)
		return again;
	VCTime left = display->cool_until - clock_wall();
	if (left > 0)
		return again != 0 && again < left ? again : left;
	part_give_back(display, display->part_cooling);
	display->part_cooling = NO_PART;
	return again;
}

/**
 * screen_clear() - Make every part black.
 * @display: the display
 */
void screen_clear(struct display *display)
{
	struct Screen *screen = display->place.screen;
	p96RectFill(&screen->RastPort, 0, 0, (UWORD)(screen->Width - 1), (UWORD)(screen->Height - 1), 0);
}
