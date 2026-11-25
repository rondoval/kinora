/* SPDX-License-Identifier: MPL-2.0 */
/*
 * gui_jump.c - the "jump to time" requester: a small window with a string
 * gadget for a time as minutes:seconds or hours:minutes:seconds.
 */
#include <stdlib.h>
#include <string.h>
#include <intuition/gadgetclass.h>
#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/string.h>
#include <gadgets/button.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/window.h>
#include <proto/layout.h>
#include <proto/string.h>
#include <proto/button.h>
#include <reaction/reaction_macros.h>

#include "gui_private.h"

#define TIME_SIZE 16 /* the time as typed, with its NUL */
#define SIXTY 60
#define MICROSECONDS_PER_SECOND 1000000

/* The requester's gadgets */
enum jump_gadget
{
	JUMP_TIME = 1,
	JUMP_GO,
	JUMP_CANCEL,
};

struct Library *StringBase;

/**
 * time_typed() - The time a text names.
 * @text: seconds, minutes:seconds or hours:minutes:seconds
 *
 * Return: the time in microseconds.
 */
static VCTime time_typed(const char *text)
{
	VCTime seconds = 0;
	while (*text != 0)
	{
		char *rest = NULL;
		seconds = seconds * SIXTY + strtol(text, &rest, 10);
		text = *rest == ':' ? rest + 1 : "";
	}
	return seconds * MICROSECONDS_PER_SECOND;
}

/**
 * requester_serve() - Serve the requester until it is answered.
 * @requester: its window object, open
 *
 * Return: TRUE when the time is to be taken: Return in the string gadget, or the Jump button.
 */
static BOOL requester_serve(Object *requester)
{
	ULONG signals = 0;
	GetAttr(WINDOW_SigMask, requester, &signals);
	for (;;)
	{
		Wait(signals);
		ULONG result;
		UWORD code = 0;
		while ((result = RA_HandleInput(requester, &code)) != WMHI_LASTMSG)
		{
			ULONG class = result & WMHI_CLASSMASK, gadget = result & WMHI_GADGETMASK;
			if (class == WMHI_CLOSEWINDOW || (class == WMHI_GADGETUP && gadget == JUMP_CANCEL))
				return FALSE;
			if (class == WMHI_GADGETUP && (gadget == JUMP_GO || gadget == JUMP_TIME))
				return TRUE;
		}
	}
}

/**
 * jump_ask() - Ask for a time and move the film there.
 * @gui: the front
 */
void jump_ask(struct gui *gui)
{
	struct Gadget *time = (struct Gadget *)NewObject(STRING_GetClass(), NULL, GA_ID, JUMP_TIME, GA_RelVerify, TRUE, STRINGA_MaxChars,
	                                                 TIME_SIZE, STRINGA_TextVal, (ULONG) "0:00", TAG_END);
	Object *buttons =
	    NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ, LAYOUT_EvenSize, TRUE, LAYOUT_AddChild,
	              (ULONG)NewObject(BUTTON_GetClass(), NULL, GA_ID, JUMP_GO, GA_RelVerify, TRUE, GA_Text, (ULONG) "Jump", TAG_END),
	              LAYOUT_AddChild,
	              (ULONG)NewObject(BUTTON_GetClass(), NULL, GA_ID, JUMP_CANCEL, GA_RelVerify, TRUE, GA_Text, (ULONG) "Cancel", TAG_END),
	              TAG_END);
	Object *root = NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_VERT, LAYOUT_SpaceOuter, TRUE, LAYOUT_AddChild,
	                         (ULONG)time, LAYOUT_AddChild, (ULONG)buttons, TAG_END);
	Object *requester =
	    NewObject(WINDOW_GetClass(), NULL, WA_Title, (ULONG) "Jump to time (minutes:seconds)", WA_DragBar, TRUE, WA_DepthGadget, TRUE,
	              WA_CloseGadget, TRUE, WA_Activate, TRUE, WA_PubScreen, (ULONG)gui->player.window->WScreen, WINDOW_Position,
	              WPOS_CENTERWINDOW, WINDOW_RefWindow, (ULONG)gui->player.window, WINDOW_ParentGroup, (ULONG)root, TAG_END);
	if (requester == NULL)
		return; /* without memory for the window its gadgets are lost with it */
	struct Window *window = (struct Window *)RA_OpenWindow(requester);
	if (window != NULL && time != NULL)
	{
		ActivateLayoutGadget((struct Gadget *)root, window, NULL, (ULONG)time);
		if (requester_serve(requester))
		{
			STRPTR typed = NULL;
			GetAttr(STRINGA_TextVal, (Object *)time, (ULONG *)&typed);
			if (typed != NULL)
				engine_seek(gui->engine, time_typed((const char *)typed));
		}
	}
	DisposeObject(requester);
}
