/* SPDX-License-Identifier: MPL-2.0 */
/*
 * gui_wb.c - the Workbench's part: icons dropped on the player's window
 * become the playlist, those dropped on the playlist window join it.
 */
#include <string.h>
#include <workbench/workbench.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/wb.h>

#include "gui_private.h"

struct Library *WorkbenchBase;

/**
 * drop_open() - Make the window one that icons can be dropped on.
 * @gui: the front, its window open
 *
 * Without workbench.library, or with no Workbench running, nothing can be dropped.
 */
void drop_open(struct gui *gui)
{
	WorkbenchBase = OpenLibrary((CONST_STRPTR) "workbench.library", 39);
	if (WorkbenchBase == NULL)
		return;
	gui->drop_port = CreateMsgPort();
	gui->drop_window = drop_add(gui, gui->player.window, DROP_PLAYER);
}

/**
 * drop_add() - Make a window one that icons can be dropped on.
 * @gui:    the front, drop_open() done
 * @window: the window
 * @place:  which window it is, for drop_take()
 *
 * Return: the AppWindow for drop_remove(), NULL when there is no Workbench or no memory.
 */
struct AppWindow *drop_add(struct gui *gui, struct Window *window, enum drop_place place)
{
	return gui->drop_port != NULL ? AddAppWindowA(place, 0, window, gui->drop_port, NULL) : NULL;
}

/**
 * drop_remove() - A window takes no more icons.
 * @drop_window: what drop_add() gave; NULL does nothing
 */
void drop_remove(struct AppWindow *drop_window)
{
	if (drop_window != NULL)
		RemoveAppWindow(drop_window);
}

/**
 * drop_close() - The window takes no more icons.
 * @gui: the front
 */
void drop_close(struct gui *gui)
{
	drop_remove(gui->drop_window);
	gui->drop_window = NULL;
	if (gui->drop_port != NULL)
	{
		struct Message *message;
		while ((message = GetMsg(gui->drop_port)) != NULL)
			ReplyMsg(message);
		DeleteMsgPort(gui->drop_port);
	}
	gui->drop_port = NULL;
	if (WorkbenchBase != NULL)
		CloseLibrary(WorkbenchBase);
	WorkbenchBase = NULL;
}

/**
 * drop_take() - Take the icons dropped: on the player's window they replace the playlist and the first plays; on the
 *               playlist window they go behind its films.
 * @gui: the front
 */
void drop_take(struct gui *gui)
{
	struct AppMessage *message;
	while (gui->drop_port != NULL && (message = (struct AppMessage *)GetMsg(gui->drop_port)) != NULL)
	{
		BOOL replace = message->am_ID == DROP_PLAYER && message->am_NumArgs > 0;
		if (replace)
			playlist_clear(&gui->playlist);
		for (LONG i = 0; i < message->am_NumArgs; i++)
		{
			char path[PATH_SIZE];
			const struct WBArg *dropped = &message->am_ArgList[i];
			if (NameFromLock(dropped->wa_Lock, (STRPTR)path, sizeof(path)) && AddPart((STRPTR)path, (CONST_STRPTR)dropped->wa_Name, sizeof(path)))
				playlist_add(&gui->playlist, (CONST_STRPTR)path);
		}
		ReplyMsg(&message->am_Message);
		if (replace)
			film_play(gui, 0);
	}
}
