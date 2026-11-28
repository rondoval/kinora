/* SPDX-License-Identifier: MPL-2.0 */
/*
 * gui_list.c - the playlist window: the films in a list, the open one
 * marked, and buttons to add, remove, move and clear them.
 *
 * The window follows the playlist by its count of changes: whatever changed
 * the playlist (the window, the menus, ARexx, a drop, a film ending), the
 * lines are made anew on the event loop's next pass.  A double click or
 * Return plays the film selected, Del takes it out, Esc closes the window;
 * the other keys do what they do in the player's window.
 */
#include <string.h>
#include <exec/memory.h>
#include <intuition/gadgetclass.h>
#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/listbrowser.h>
#include <gadgets/space.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/window.h>
#include <proto/layout.h>
#include <proto/listbrowser.h>
#include <proto/space.h>
#include <reaction/reaction_macros.h>

#include "gui_private.h"

#define LIST_WIDTH 480     /* the window's inner size the first time it opens */
#define LIST_HEIGHT 280
#define LIST_COLUMNS 3u    /* the mark, the film's name, its drawer */
#define MARK_OPEN "\273"   /* the open film's mark: a right guillemet in Latin-1 */
#define RAWKEY_RETURN 0x44
#define RAWKEY_ESCAPE 0x45
#define RAWKEY_DELETE 0x46

struct Library *ListBrowserBase;

/* The list's columns, by weight: the mark, the name, the drawer */
static struct ColumnInfo columns[] = {
    {4, (STRPTR) "", CIF_WEIGHTED},
    {60, (STRPTR) "Film", CIF_WEIGHTED},
    {36, (STRPTR) "Drawer", CIF_WEIGHTED},
    {-1, (STRPTR)~0, (ULONG)-1},
};

/**
 * line_make() - Make the list's line for one film.
 * @playlist: the playlist
 * @index:    the film
 *
 * Return: the node, NULL without memory.
 */
static struct Node *line_make(const struct playlist *playlist, ULONG index)
{
	char drawer[PATH_SIZE];
	strncpy(drawer, playlist->films[index], sizeof(drawer) - 1);
	drawer[sizeof(drawer) - 1] = 0;
	*PathPart((CONST_STRPTR)drawer) = 0;
	BOOL open = index == playlist->current && !playlist->open_gone;
	return AllocListBrowserNode(LIST_COLUMNS,
	                            LBNA_Column, 0, LBNCA_Text, (ULONG)(open ? MARK_OPEN : ""),
	                            LBNA_Column, 1, LBNCA_CopyText, TRUE, LBNCA_Text, (ULONG)playlist_name(playlist, index),
	                            LBNA_Column, 2, LBNCA_CopyText, TRUE, LBNCA_Text, (ULONG)drawer,
	                            TAG_DONE);
}

/**
 * lines_make() - Make the list's lines for the playlist as it is now.
 * @list:     the window
 * @playlist: the playlist
 *
 * Without memory the list stops short.
 */
static void lines_make(struct playlist_window *list, const struct playlist *playlist)
{
	NewList(&list->lines);
	for (ULONG i = 0; i < playlist->count; i++)
	{
		struct Node *node = line_make(playlist, i);
		if (node == NULL)
			break;
		AddTail(&list->lines, node);
	}
	list->shown_changes = playlist->changes;
	if (list->selected >= (LONG)playlist->count)
		list->selected = (LONG)playlist->count - 1;
}

/**
 * playlist_window_follow() - Show the playlist anew if it changed since its lines were made.
 * @gui: the front; nothing happens without the window
 */
void playlist_window_follow(struct gui *gui)
{
	struct playlist_window *list = &gui->playlist_window;
	if (list->window == NULL || list->shown_changes == gui->playlist.changes)
		return;
	struct Gadget *films = list->gadgets[LIST_FILMS];
	SetGadgetAttrs(films, list->window, NULL, LISTBROWSER_Labels, ~0UL, TAG_DONE);
	FreeListBrowserList(&list->lines);
	lines_make(list, &gui->playlist);
	SetGadgetAttrs(films, list->window, NULL, LISTBROWSER_Labels, (ULONG)&list->lines, LISTBROWSER_Selected, (ULONG)list->selected,
	               LISTBROWSER_MakeVisible, (ULONG)(list->selected >= 0 ? list->selected : 0), TAG_DONE);
	RefreshGList(films, list->window, NULL, 1);
}

/**
 * root_make() - Make the window's gadgets and their layout.
 * @list: the window, its lines made
 *
 * Return: the layout, NULL when a gadget could not be made; those made are freed then.
 */
static Object *root_make(struct playlist_window *list)
{
	struct Gadget **gadgets = list->gadgets;
	gadgets[LIST_FILMS] = (struct Gadget *)NewObject(LISTBROWSER_GetClass(), NULL,
	                                                 GA_ID, LIST_FILMS,
	                                                 GA_RelVerify, TRUE,
	                                                 LISTBROWSER_ColumnInfo, (ULONG)columns,
	                                                 LISTBROWSER_ColumnTitles, TRUE,
	                                                 LISTBROWSER_ShowSelected, TRUE,
	                                                 LISTBROWSER_Labels, (ULONG)&list->lines,
	                                                 LISTBROWSER_Selected, (ULONG)list->selected,
	                                                 TAG_END);
	gadgets[LIST_ADD] = button_make(LIST_ADD, (CONST_STRPTR) "Add...");
	gadgets[LIST_REMOVE] = button_make(LIST_REMOVE, (CONST_STRPTR) "Remove");
	gadgets[LIST_UP] = button_make(LIST_UP, (CONST_STRPTR) "Up");
	gadgets[LIST_DOWN] = button_make(LIST_DOWN, (CONST_STRPTR) "Down");
	gadgets[LIST_CLEAR] = button_make(LIST_CLEAR, (CONST_STRPTR) "Clear");
	for (ULONG id = LIST_FILMS; id < LIST_COUNT; id++)
		if (gadgets[id] == NULL)
		{
			for (ULONG made = LIST_FILMS; made < LIST_COUNT; made++)
				DisposeObject((Object *)gadgets[made]); /* NULL is passed over */
			return NULL;
		}
	Object *row = NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
	                        LAYOUT_AddChild, (ULONG)gadgets[LIST_ADD], CHILD_WeightedWidth, 0,
	                        LAYOUT_AddChild, (ULONG)gadgets[LIST_REMOVE], CHILD_WeightedWidth, 0,
	                        LAYOUT_AddChild, (ULONG)NewObject(SPACE_GetClass(), NULL, TAG_END),
	                        LAYOUT_AddChild, (ULONG)gadgets[LIST_UP], CHILD_WeightedWidth, 0,
	                        LAYOUT_AddChild, (ULONG)gadgets[LIST_DOWN], CHILD_WeightedWidth, 0,
	                        LAYOUT_AddChild, (ULONG)NewObject(SPACE_GetClass(), NULL, TAG_END),
	                        LAYOUT_AddChild, (ULONG)gadgets[LIST_CLEAR], CHILD_WeightedWidth, 0,
	                        TAG_END);
	return NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_VERT, LAYOUT_SpaceOuter, TRUE,
	                 LAYOUT_AddChild, (ULONG)gadgets[LIST_FILMS],
	                 LAYOUT_AddChild, (ULONG)row, CHILD_WeightedHeight, 0,
	                 TAG_END);
}

/**
 * window_make() - Make the window object around the layout.
 * @root: the layout
 * @box:  where the window last stood and its size; Width 0 for in the middle of the screen at LIST_WIDTH by LIST_HEIGHT
 *
 * Return: the window.class object, NULL without memory.
 */
static Object *window_make(Object *root, const struct IBox *box)
{
	const struct TagItem placed[] = {
	    {WA_Left, (ULONG)(LONG)box->Left}, {WA_Top, (ULONG)(LONG)box->Top},
	    {WA_Width, (ULONG)(LONG)box->Width}, {WA_Height, (ULONG)(LONG)box->Height}, {TAG_DONE, 0},
	};
	const struct TagItem centred[] = {
	    {WA_InnerWidth, LIST_WIDTH}, {WA_InnerHeight, LIST_HEIGHT}, {WINDOW_Position, WPOS_CENTERSCREEN}, {TAG_DONE, 0},
	};
	return NewObject(WINDOW_GetClass(), NULL,
	                 WA_Title, (ULONG) "Kinora: Playlist",
	                 WA_DragBar, TRUE,
	                 WA_DepthGadget, TRUE,
	                 WA_CloseGadget, TRUE,
	                 WA_SizeGadget, TRUE,
	                 WA_Activate, TRUE,
	                 WA_IDCMP, IDCMP_RAWKEY,
	                 WINDOW_ParentGroup, (ULONG)root,
	                 TAG_MORE, (ULONG)(box->Width > 0 ? placed : centred));
}

/**
 * playlist_window_open() - Open the playlist window, or bring it to the front.
 * @gui: the front
 *
 * It opens where it last stood, at its size then; the first time in the
 * middle of the screen.  The open film is selected.  Without memory the
 * screen flashes.
 */
void playlist_window_open(struct gui *gui)
{
	struct playlist_window *list = &gui->playlist_window;
	if (list->window != NULL)
	{
		WindowToFront(list->window);
		ActivateWindow(list->window);
		return;
	}
	list->selected = gui->playlist.count > 0 && !gui->playlist.open_gone ? (LONG)gui->playlist.current : -1;
	lines_make(list, &gui->playlist);
	Object *root = root_make(list);
	if (root != NULL)
		list->window_object = window_make(root, &gui->settings.playlist_box);
	if (list->window_object == NULL)
	{
		DisposeObject(root); /* NULL is passed over */
		FreeListBrowserList(&list->lines);
		DisplayBeep(gui->player.window->WScreen);
		return;
	}
	list->window = (struct Window *)RA_OpenWindow(list->window_object);
	if (list->window == NULL)
	{
		playlist_window_close(gui);
		DisplayBeep(gui->player.window->WScreen);
		return;
	}
	list->drop_window = drop_add(gui, list->window, DROP_PLAYLIST);
}

/**
 * playlist_window_close() - Close the playlist window, its place and size remembered.
 * @gui: the front; nothing happens without the window
 */
void playlist_window_close(struct gui *gui)
{
	struct playlist_window *list = &gui->playlist_window;
	if (list->window_object == NULL)
		return;
	if (list->window != NULL)
	{
		const struct Window *window = list->window;
		gui->settings.playlist_box = (struct IBox){.Left = window->LeftEdge, .Top = window->TopEdge, .Width = window->Width, .Height = window->Height};
	}
	drop_remove(list->drop_window);
	DisposeObject(list->window_object); /* closes the window, frees the gadgets; the lines are the list's */
	FreeListBrowserList(&list->lines);
	*list = (struct playlist_window){.selected = -1};
}

/**
 * selected_play() - Play the film selected.
 * @gui: the front
 */
static void selected_play(struct gui *gui)
{
	if (gui->playlist_window.selected >= 0)
		film_play(gui, (ULONG)gui->playlist_window.selected);
}

/**
 * selected_change() - Take the film selected out, or move it up or down.
 * @gui: the front
 * @id:  LIST_REMOVE, LIST_UP or LIST_DOWN
 *
 * The selection stays on the moved film, or on the place of the one taken out.
 */
static void selected_change(struct gui *gui, ULONG id)
{
	struct playlist_window *list = &gui->playlist_window;
	if (list->selected < 0)
		return;
	if (id == LIST_REMOVE)
		playlist_remove(&gui->playlist, (ULONG)list->selected);
	else
		list->selected = (LONG)playlist_move(&gui->playlist, (ULONG)list->selected, id == LIST_UP ? -1 : 1);
}

/**
 * list_gadget_used() - Act on one of the window's gadgets.
 * @gui: the front
 * @id:  the gadget's number
 */
static void list_gadget_used(struct gui *gui, ULONG id)
{
	struct playlist_window *list = &gui->playlist_window;
	ULONG value = 0;
	switch (id)
	{
	case LIST_FILMS:
		GetAttr(LISTBROWSER_Selected, (Object *)list->gadgets[LIST_FILMS], &value);
		list->selected = (LONG)value;
		GetAttr(LISTBROWSER_RelEvent, (Object *)list->gadgets[LIST_FILMS], &value);
		if ((value & LBRE_DOUBLECLICK) != 0)
			selected_play(gui);
		break;
	case LIST_ADD:
		films_ask(gui, TRUE);
		break;
	case LIST_REMOVE:
	case LIST_UP:
	case LIST_DOWN:
		selected_change(gui, id);
		break;
	case LIST_CLEAR:
		playlist_clear(&gui->playlist);
		break;
	default:
		break;
	}
}

/**
 * list_key_used() - Act on a key pressed in the window.
 * @gui:  the front
 * @code: the raw key
 */
static void list_key_used(struct gui *gui, ULONG code)
{
	switch (code)
	{
	case RAWKEY_RETURN:
		selected_play(gui);
		break;
	case RAWKEY_DELETE:
		list_gadget_used(gui, LIST_REMOVE);
		break;
	case RAWKEY_ESCAPE:
		playlist_window_close(gui);
		break;
	default:
		key_used(gui, code);
		break;
	}
}

/**
 * playlist_window_input_take() - Take what the playlist window has to say.
 * @gui: the front; nothing happens without the window
 *
 * A key or gadget may close the window: it is looked at anew for every message.
 */
void playlist_window_input_take(struct gui *gui)
{
	struct playlist_window *list = &gui->playlist_window;
	ULONG result;
	UWORD code = 0;
	while (!gui->quit && list->window_object != NULL && (result = RA_HandleInput(list->window_object, &code)) != WMHI_LASTMSG)
	{
		switch (result & WMHI_CLASSMASK)
		{
		case WMHI_CLOSEWINDOW:
			playlist_window_close(gui);
			break;
		case WMHI_GADGETUP:
			list_gadget_used(gui, result & WMHI_GADGETMASK);
			break;
		case WMHI_RAWKEY:
			list_key_used(gui, result & WMHI_KEYMASK);
			break;
		default:
			break;
		}
	}
}
