/* SPDX-License-Identifier: MPL-2.0 */
/*
 * gui_wb.c - the Workbench's part: icons dropped on the window become films
 * of the playlist.
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
	if (gui->drop_port != NULL)
		gui->drop_window = AddAppWindowA(0, 0, gui->player.window, gui->drop_port, NULL);
}

/**
 * drop_close() - The window takes no more icons.
 * @gui: the front
 */
void drop_close(struct gui *gui)
{
	if (gui->drop_window != NULL)
		RemoveAppWindow(gui->drop_window);
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
 * drop_take() - Take the icons dropped on the window: they replace the playlist and the first plays.
 * @gui: the front
 */
void drop_take(struct gui *gui)
{
	struct AppMessage *message;
	while (gui->drop_port != NULL && (message = (struct AppMessage *)GetMsg(gui->drop_port)) != NULL)
	{
		if (message->am_NumArgs > 0)
			playlist_clear(&gui->playlist);
		for (LONG i = 0; i < message->am_NumArgs; i++)
		{
			char path[PATH_SIZE];
			const struct WBArg *dropped = &message->am_ArgList[i];
			if (NameFromLock(dropped->wa_Lock, (STRPTR)path, sizeof(path)) && AddPart((STRPTR)path, (CONST_STRPTR)dropped->wa_Name, sizeof(path)))
				playlist_add(&gui->playlist, (CONST_STRPTR)path);
		}
		ReplyMsg(&message->am_Message);
		film_play(gui, 0);
	}
}
