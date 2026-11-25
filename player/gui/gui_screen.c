/* SPDX-License-Identifier: MPL-2.0 */
/*
 * gui_screen.c - full screen: the picture on a screen of its own, and the
 * controls shown over its foot while the pointer moves.
 *
 * The screen is in the Workbench's mode and up to three pictures tall; the
 * engine writes the pictures into it and scrolls (display_screen.c).  A
 * borderless window over its first picture height takes the keys and the
 * pointer's moves.
 *
 * A moving pointer brings up a strip of controls along the screen's foot.
 * While it shows, the picture stands in the screen's first picture height
 * only, above the strip, and is not scrolled: a window can be drawn over one
 * place of the bitmap, not over a picture that moves between three.  When
 * the pointer has been still for a few seconds the strip goes and the
 * picture has the whole screen again.
 */
#include <exec/memory.h>
#include <intuition/screens.h>
#include <libraries/Picasso96.h>
#include <classes/window.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/Picasso96.h>

#include "gui_private.h"

#define POINTER_WORDS 6 /* an empty pointer image: two control words, one line, two end words */
#define STRIP_IDLE_TICKS 12u /* ticks with the pointer still and no control used after which the strip goes: three seconds */

/**
 * full_screen_place() - Where on the full screen the picture goes: all of it, or above the strip.
 * @gui:   the front, the full screen open
 * @place: filled in; with the strip up, the picture's rows are rounded down to DISPLAY_EVEN_STEP and the rest are the foot
 */
void full_screen_place(const struct gui *gui, struct display_place *place)
{
	*place = (struct display_place){.screen = gui->screen, .width = (LONG)gui->screen_width, .height = (LONG)gui->screen_height};
	if (gui->strip.window == NULL)
		return;
	ULONG above = gui->screen_height - (ULONG)gui->strip.window->Height;
	place->height = (LONG)(above / DISPLAY_EVEN_STEP * DISPLAY_EVEN_STEP);
	place->foot = (LONG)gui->screen_height - place->height;
}

/**
 * place_set() - Tell the engine where on the full screen the picture goes.
 * @gui: the front, the full screen open
 *
 * Return: 0, or the engine's refusal of the screen.
 */
static LONG place_set(struct gui *gui)
{
	struct display_place place;
	full_screen_place(gui, &place);
	return engine_place(gui->engine, &place);
}

/**
 * strip_hide() - Take the controls off the full screen and give the picture the whole of it.
 * @gui:          the front
 * @screen_stays: the screen stays: tell the engine its new place
 */
static void strip_hide(struct gui *gui, BOOL screen_stays)
{
	if (gui->strip.window_object == NULL)
		return;
	controls_free(&gui->strip);
	if (gui->blank_pointer != NULL)
		SetPointer(gui->screen_window, gui->blank_pointer, 1, 16, 0, 0);
	if (screen_stays)
		place_set(gui);
}

/**
 * strip_show() - Bring the controls up along the full screen's foot.
 * @gui: the front, the full screen open
 */
static void strip_show(struct gui *gui)
{
	gui->strip_idle_ticks = 0;
	if (gui->strip.window_object != NULL)
		return;
	if (!controls_make(&gui->strip, gui->screen, gui->settings.volume))
	{
		controls_free(&gui->strip);
		return;
	}
	struct Window *window = gui->strip.window;
	ChangeWindowBox(window, 0, (WORD)((LONG)gui->screen_height - window->Height), (WORD)gui->screen_width, window->Height);
	ClearPointer(gui->screen_window);
	place_set(gui);
	film_follow(gui);
}

/**
 * full_screen_close() - Close the full screen, its strip and its window.
 * @gui: the front; the engine no longer shows pictures on the screen
 */
void full_screen_close(struct gui *gui)
{
	strip_hide(gui, FALSE);
	if (gui->screen_window != NULL)
	{
		ClearPointer(gui->screen_window);
		CloseWindow(gui->screen_window);
	}
	gui->screen_window = NULL;
	if (gui->blank_pointer != NULL)
		FreeMem(gui->blank_pointer, POINTER_WORDS * sizeof(UWORD));
	gui->blank_pointer = NULL;
	if (gui->screen != NULL)
		p96CloseScreen(gui->screen);
	gui->screen = NULL;
}

/**
 * full_screen_open() - Open a screen in the Workbench's mode, as many pictures tall as the card's memory allows.
 * @gui: the front
 *
 * The screen is black, and a borderless window on it takes the keys and the
 * pointer's moves under an empty pointer.
 *
 * Return: TRUE, FALSE when no screen or window opened.
 */
static BOOL full_screen_open(struct gui *gui)
{
	ULONG mode = (ULONG)GetVPModeID(&gui->player.window->WScreen->ViewPort);
	ULONG depth = p96GetModeIDAttr(mode, P96IDA_DEPTH);
	gui->screen_width = p96GetModeIDAttr(mode, P96IDA_WIDTH);
	gui->screen_height = p96GetModeIDAttr(mode, P96IDA_HEIGHT);
	for (ULONG parts = DISPLAY_PARTS; parts >= 1 && gui->screen == NULL; parts--)
		gui->screen = p96OpenScreenTags(P96SA_DisplayID, mode,
		                                P96SA_Width, gui->screen_width,
		                                P96SA_Height, gui->screen_height * parts,
		                                P96SA_Depth, depth,
		                                P96SA_Quiet, TRUE,
		                                P96SA_ShowTitle, FALSE,
		                                P96SA_AutoScroll, FALSE,
		                                P96SA_Title, (ULONG) "Kinora",
		                                TAG_DONE);
	if (gui->screen == NULL)
		return FALSE;
	p96RectFill(&gui->screen->RastPort, 0, 0, (UWORD)(gui->screen->Width - 1), (UWORD)(gui->screen->Height - 1), 0);
	gui->screen_window = OpenWindowTags(NULL,
	                                    WA_CustomScreen, (ULONG)gui->screen,
	                                    WA_Left, 0,
	                                    WA_Top, 0,
	                                    WA_Width, gui->screen_width,
	                                    WA_Height, gui->screen_height,
	                                    WA_Backdrop, TRUE,
	                                    WA_Borderless, TRUE,
	                                    WA_Activate, TRUE,
	                                    WA_RMBTrap, TRUE,
	                                    WA_ReportMouse, TRUE,
	                                    WA_IDCMP, IDCMP_RAWKEY | IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS,
	                                    TAG_DONE);
	if (gui->screen_window == NULL)
		return FALSE;
	gui->blank_pointer = AllocMem(POINTER_WORDS * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR);
	if (gui->blank_pointer != NULL)
		SetPointer(gui->screen_window, gui->blank_pointer, 1, 16, 0, 0);
	return TRUE;
}

/**
 * full_screen_toggle() - Move the picture to a screen of its own, or back into the window.
 * @gui: the front
 *
 * The engine is given the new place before the old one closes.  A screen
 * the engine cannot write to is closed again and the picture stays in the window.
 */
void full_screen_toggle(struct gui *gui)
{
	if (gui->screen == NULL && full_screen_open(gui) && place_set(gui) == 0)
		return;
	struct display_place place;
	picture_box(gui, &place);
	engine_place(gui->engine, &place);
	full_screen_close(gui);
}

/**
 * full_screen_signals() - What the full screen's windows signal the front with.
 * @gui: the front
 *
 * Return: the signal bits, 0 without a full screen.
 */
ULONG full_screen_signals(const struct gui *gui)
{
	ULONG signals = 0;
	if (gui->strip.window_object != NULL)
		GetAttr(WINDOW_SigMask, gui->strip.window_object, &signals);
	if (gui->screen_window != NULL)
		signals |= 1UL << gui->screen_window->UserPort->mp_SigBit;
	return signals;
}

/**
 * full_screen_input_take() - Take the keys typed on the full screen, the pointer's moves and clicks, and what the strip says.
 * @gui: the front
 *
 * A key may close the screen, and its window with it: the port is looked up anew for every message.
 */
void full_screen_input_take(struct gui *gui)
{
	struct IntuiMessage *message;
	while (gui->screen_window != NULL && (message = (struct IntuiMessage *)GetMsg(gui->screen_window->UserPort)) != NULL)
	{
		ULONG class = message->Class;
		UWORD code = message->Code;
		BOOL moved = message->MouseX != gui->pointer_x || message->MouseY != gui->pointer_y;
		gui->pointer_x = message->MouseX;
		gui->pointer_y = message->MouseY;
		ReplyMsg(&message->ExecMessage);
		if (class == IDCMP_RAWKEY)
			key_used(gui, code);
		else if (class == IDCMP_MOUSEBUTTONS && code == SELECTDOWN && click_doubled(gui))
			full_screen_toggle(gui); /* a double click on the full screen: back into the window */
		else if (class == IDCMP_MOUSEMOVE && moved) /* an event that moves nothing (another program's) shows no strip */
			strip_show(gui);
	}
	controls_input_take(gui, &gui->strip);
}

/**
 * full_screen_tick() - A tick has passed: take the strip away once the pointer has been still and no control used for long enough.
 * @gui: the front
 */
void full_screen_tick(struct gui *gui)
{
	if (gui->strip.window_object != NULL && ++gui->strip_idle_ticks >= STRIP_IDLE_TICKS)
		strip_hide(gui, TRUE);
}
