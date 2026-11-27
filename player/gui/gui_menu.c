/* SPDX-License-Identifier: MPL-2.0 */
/*
 * gui_menu.c - the window's menus.
 *
 * Besides the fixed items the menus list what changes: the film's audio and
 * subtitle tracks and its chapters, the sound units, the playlist.  So they
 * are made again, from a table filled anew, whenever one of those may have
 * changed: after a film opens and after every item picked.
 */
#include <stdio.h>
#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>

#include "gui_private.h"

#define MENU_ITEMS 128u /* the items the table holds */
#define AHI_UNITS 4u    /* ahi.device has units 0 to 3 */
#define FILMS_LISTED 24u /* the playlist's films the menu lists */
#define CHAPTERS_LISTED 30u /* the chapters the Chapters submenu lists */
#define AUDIO_STEP 50       /* milliseconds one pick moves the sound */
#define SUBTITLE_STEP 250   /* and the subtitles */
#define MICROSECONDS_PER_SECOND 1000000
#define SIXTY 60
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

_Static_assert(CHAPTERS_LISTED <= NOSUB, "Intuition numbers an item's subitems 0 to NOSUB - 1");

/* A shape the picture can be shown in */
struct aspect
{
	const char *label; /* its menu item */
	LONG width;        /* the shape's width, 0 for the film's own, DISPLAY_ASPECT_FILL to stretch */
	LONG height;       /* to this height */
};

static const struct aspect aspects[] = {
    {"Shape: the film's own", 0, 0},
    {"Shape: 4:3", 4, 3},
    {"Shape: 16:9", 16, 9},
    {"Shape: 2.35:1", 235, 100},
    {"Shape: stretched to fit", DISPLAY_ASPECT_FILL, 0},
};

/* The menu items (nm_UserData); the ranges hold a number in their low bits */
enum menu_id
{
	MENU_OPEN = 1,
	MENU_ADD,
	MENU_SUBTITLE_OPEN,
	MENU_CLOSE,
	MENU_QUIT,
	MENU_FULL_SCREEN,
	MENU_PLAY,
	MENU_STOP,
	MENU_NEXT,
	MENU_PREVIOUS,
	MENU_SHARED,
	MENU_SUBTITLE_OFF,
	MENU_JUMP,
	MENU_AUDIO_LATER,
	MENU_AUDIO_EARLIER,
	MENU_SUBTITLES_LATER,
	MENU_SUBTITLES_EARLIER,
	MENU_NO_DELAYS,
	MENU_FONT,
	MENU_FONT_OWN,
	MENU_STATISTICS,
	MENU_TRACK = 0x100, /* + the track's place in gui->tracks */
	MENU_UNIT = 0x200,  /* + the unit */
	MENU_FILM = 0x300,  /* + the film's place in the playlist */
	MENU_CHAPTER = 0x400, /* + the chapter's place in gui->chapters */
	MENU_ASPECT = 0x500,  /* + the shape's place in aspects[] */
	MENU_RANGE = 0xff00
};

/* The table being filled */
struct menu_table
{
	struct gui *gui; /* whose menus */
	ULONG count;     /* the items filled */
};

/**
 * item_add() - Add an item to the table.
 * @table:    the table
 * @type:     NM_TITLE, NM_ITEM or NM_SUB
 * @label:    its text, copied; NM_BARLABEL makes a bar
 * @key:      its shortcut, NULL for none
 * @id:       its menu_id
 * @checked:  it is one of several of which this is the chosen: a checkmark, set or not; -1 for a plain item
 */
static void item_add(struct menu_table *table, UBYTE type, CONST_STRPTR label, CONST_STRPTR key, ULONG id, LONG checked)
{
	if (table->count + 1 >= MENU_ITEMS)
		return;
	struct NewMenu *item = &table->gui->new_menus[table->count];
	*item = (struct NewMenu){.nm_Type = type, .nm_Label = (STRPTR)label, .nm_CommKey = (STRPTR)key, .nm_UserData = (APTR)id};
	if (checked >= 0)
		item->nm_Flags = (UWORD)(CHECKIT | (checked != 0 ? CHECKED : 0));
	if (label != NM_BARLABEL)
	{
		char *copy = table->gui->labels[table->count];
		strncpy(copy, (const char *)label, MENU_LABEL_SIZE - 1);
		copy[MENU_LABEL_SIZE - 1] = 0;
		item->nm_Label = (STRPTR)copy;
	}
	table->count++;
}

/**
 * tracks_add() - Add the film's tracks of one kind.
 * @table: the table
 * @kind:  audio or subtitle
 */
static void tracks_add(struct menu_table *table, enum engine_track_kind kind)
{
	const struct gui *gui = table->gui;
	for (ULONG i = 0; i < gui->track_count; i++)
	{
		const struct engine_track *track = &gui->tracks[i];
		if (track->kind != kind)
			continue;
		char label[MENU_LABEL_SIZE];
		if (kind == ENGINE_TRACK_AUDIO)
			snprintf(label, sizeof(label), "%ld: %s %s (%lu channels)", (long)track->stream, track->language, track->title,
			         (unsigned long)track->channels);
		else
			snprintf(label, sizeof(label), "%s %s", track->language, track->title);
		item_add(table, NM_ITEM, (CONST_STRPTR)label, NULL, MENU_TRACK + i, track->selected);
	}
}

/**
 * subtitles_shown() - Whether one of the film's subtitle tracks is shown.
 * @gui: the front
 *
 * Return: TRUE if one is.
 */
static BOOL subtitles_shown(const struct gui *gui)
{
	for (ULONG i = 0; i < gui->track_count; i++)
		if (gui->tracks[i].kind == ENGINE_TRACK_SUBTITLE && gui->tracks[i].selected)
			return TRUE;
	return FALSE;
}

/**
 * sound_add() - Add the Audio menu: the film's tracks, then the units.
 * @table: the table
 */
static void sound_add(struct menu_table *table)
{
	const struct settings *settings = &table->gui->settings;
	item_add(table, NM_TITLE, (CONST_STRPTR) "Audio", NULL, 0, -1);
	tracks_add(table, ENGINE_TRACK_AUDIO);
	item_add(table, NM_ITEM, NM_BARLABEL, NULL, 0, -1);
	for (ULONG unit = 0; unit < AHI_UNITS; unit++)
	{
		char label[MENU_LABEL_SIZE];
		snprintf(label, sizeof(label), "AHI unit %lu", (unsigned long)unit);
		item_add(table, NM_ITEM, (CONST_STRPTR)label, NULL, MENU_UNIT + unit, settings->audio_unit == unit);
	}
	item_add(table, NM_ITEM, (CONST_STRPTR) "Share the unit with other programs", NULL, MENU_SHARED, settings->shared);
}

/**
 * films_add() - Add the Playlist menu: its films, the open one checked.
 * @table: the table
 */
static void films_add(struct menu_table *table)
{
	const struct playlist *playlist = &table->gui->playlist;
	item_add(table, NM_TITLE, (CONST_STRPTR) "Playlist", NULL, 0, -1);
	item_add(table, NM_ITEM, (CONST_STRPTR) "Next", (CONST_STRPTR) "N", MENU_NEXT, -1);
	item_add(table, NM_ITEM, (CONST_STRPTR) "Previous", (CONST_STRPTR) "B", MENU_PREVIOUS, -1);
	if (playlist->count != 0)
		item_add(table, NM_ITEM, NM_BARLABEL, NULL, 0, -1);
	for (ULONG i = 0; i < playlist->count && i < FILMS_LISTED; i++)
		item_add(table, NM_ITEM, playlist_name(playlist, i), NULL, MENU_FILM + i, i == playlist->current);
}

/**
 * view_add() - Add the View menu: full screen and the shapes.
 * @table: the table
 */
static void view_add(struct menu_table *table)
{
	const struct gui *gui = table->gui;
	item_add(table, NM_TITLE, (CONST_STRPTR) "View", NULL, 0, -1);
	item_add(table, NM_ITEM, (CONST_STRPTR) "Full screen", (CONST_STRPTR) "F", MENU_FULL_SCREEN, -1);
	item_add(table, NM_ITEM, (CONST_STRPTR) "Statistics", (CONST_STRPTR) "I", MENU_STATISTICS, gui->statistics_on);
	item_add(table, NM_ITEM, NM_BARLABEL, NULL, 0, -1);
	for (ULONG i = 0; i < ARRAY_COUNT(aspects); i++)
		item_add(table, NM_ITEM, (CONST_STRPTR)aspects[i].label, NULL, MENU_ASPECT + i,
		         aspects[i].width == gui->aspect_width && aspects[i].height == gui->aspect_height);
}

/**
 * chapters_add() - Add the film's chapters as the Chapters item's submenu; no item when it has none.
 * @table: the table
 */
static void chapters_add(struct menu_table *table)
{
	struct gui *gui = table->gui;
	gui->chapter_count = engine_chapters(gui->engine, gui->chapters, ENGINE_CHAPTERS);
	if (gui->chapter_count == 0)
		return;
	item_add(table, NM_ITEM, (CONST_STRPTR) "Chapters", NULL, 0, -1);
	for (ULONG i = 0; i < gui->chapter_count && i < CHAPTERS_LISTED; i++)
	{
		char label[MENU_LABEL_SIZE];
		long seconds = (long)(gui->chapters[i].start / MICROSECONDS_PER_SECOND);
		snprintf(label, sizeof(label), "%ld:%02ld %s", seconds / SIXTY, seconds % SIXTY, gui->chapters[i].title);
		item_add(table, NM_SUB, (CONST_STRPTR)label, NULL, MENU_CHAPTER + i, -1);
	}
}

/**
 * play_add() - Add the Play menu: the transport, the jump, the chapters and the delays.
 * @table: the table
 */
static void play_add(struct menu_table *table)
{
	struct gui *gui = table->gui;
	char label[MENU_LABEL_SIZE];
	item_add(table, NM_TITLE, (CONST_STRPTR) "Play", NULL, 0, -1);
	item_add(table, NM_ITEM, (CONST_STRPTR) "Play or pause", (CONST_STRPTR) "P", MENU_PLAY, -1);
	item_add(table, NM_ITEM, (CONST_STRPTR) "Stop", NULL, MENU_STOP, -1);
	item_add(table, NM_ITEM, (CONST_STRPTR) "Jump to time...", (CONST_STRPTR) "J", MENU_JUMP, -1);
	chapters_add(table);
	item_add(table, NM_ITEM, NM_BARLABEL, NULL, 0, -1);
	item_add(table, NM_ITEM, (CONST_STRPTR) "Sound 50 ms later", (CONST_STRPTR) "+", MENU_AUDIO_LATER, -1);
	item_add(table, NM_ITEM, (CONST_STRPTR) "Sound 50 ms earlier", (CONST_STRPTR) "-", MENU_AUDIO_EARLIER, -1);
	item_add(table, NM_ITEM, (CONST_STRPTR) "Subtitles 250 ms later", NULL, MENU_SUBTITLES_LATER, -1);
	item_add(table, NM_ITEM, (CONST_STRPTR) "Subtitles 250 ms earlier", NULL, MENU_SUBTITLES_EARLIER, -1);
	snprintf(label, sizeof(label), "No delays (sound %ld ms, subtitles %ld ms)", (long)gui->audio_delay, (long)gui->subtitle_delay);
	item_add(table, NM_ITEM, (CONST_STRPTR)label, NULL, MENU_NO_DELAYS, -1);
}

/**
 * table_fill() - Fill the table with the menus as they are now.
 * @gui: the front
 */
static void table_fill(struct gui *gui)
{
	struct menu_table table = {.gui = gui};
	item_add(&table, NM_TITLE, (CONST_STRPTR) "Project", NULL, 0, -1);
	item_add(&table, NM_ITEM, (CONST_STRPTR) "Open...", (CONST_STRPTR) "O", MENU_OPEN, -1);
	item_add(&table, NM_ITEM, (CONST_STRPTR) "Add to the playlist...", (CONST_STRPTR) "A", MENU_ADD, -1);
	item_add(&table, NM_ITEM, (CONST_STRPTR) "Open subtitles...", (CONST_STRPTR) "S", MENU_SUBTITLE_OPEN, -1);
	item_add(&table, NM_ITEM, (CONST_STRPTR) "Close", NULL, MENU_CLOSE, -1);
	item_add(&table, NM_ITEM, NM_BARLABEL, NULL, 0, -1);
	item_add(&table, NM_ITEM, (CONST_STRPTR) "Quit", (CONST_STRPTR) "Q", MENU_QUIT, -1);
	view_add(&table);
	play_add(&table);
	sound_add(&table);
	item_add(&table, NM_TITLE, (CONST_STRPTR) "Subtitles", NULL, 0, -1);
	item_add(&table, NM_ITEM, (CONST_STRPTR) "Off", NULL, MENU_SUBTITLE_OFF, !subtitles_shown(gui));
	tracks_add(&table, ENGINE_TRACK_SUBTITLE);
	item_add(&table, NM_ITEM, NM_BARLABEL, NULL, 0, -1);
	item_add(&table, NM_ITEM, (CONST_STRPTR) "Font...", NULL, MENU_FONT, -1);
	item_add(&table, NM_ITEM, (CONST_STRPTR) "Kinora's font", NULL, MENU_FONT_OWN, gui->settings.font[0] == 0);
	films_add(&table);
	gui->new_menus[table.count] = (struct NewMenu){.nm_Type = NM_END};
}

/**
 * menus_free() - Take the menus off the window and free them.
 * @gui: the front
 */
void menus_free(struct gui *gui)
{
	if (gui->menus != NULL)
	{
		ClearMenuStrip(gui->player.window);
		FreeMenus(gui->menus);
	}
	gui->menus = NULL;
}

/**
 * menus_make() - Make the window's menus for the film, its tracks and the playlist as they are now.
 * @gui: the front, its window open
 *
 * Without memory the window has no menus; the keys and the gadgets still work.
 */
void menus_make(struct gui *gui)
{
	menus_free(gui);
	if (gui->new_menus == NULL)
		gui->new_menus = AllocVec(MENU_ITEMS * sizeof(struct NewMenu), MEMF_ANY);
	if (gui->labels == NULL)
		gui->labels = AllocVec(MENU_ITEMS * MENU_LABEL_SIZE, MEMF_ANY);
	if (gui->visual_info == NULL)
		gui->visual_info = GetVisualInfoA(gui->player.window->WScreen, NULL);
	if (gui->new_menus == NULL || gui->labels == NULL || gui->visual_info == NULL)
		return;
	gui->track_count = engine_tracks(gui->engine, gui->tracks, ENGINE_TRACKS);
	table_fill(gui);
	gui->menus = CreateMenusA(gui->new_menus, NULL);
	if (gui->menus != NULL && LayoutMenus(gui->menus, gui->visual_info, GTMN_NewLookMenus, TRUE, TAG_DONE))
		SetMenuStrip(gui->player.window, gui->menus);
	else
		menus_free(gui);
}

/**
 * range_used() - Act on an item of one of the changing lists.
 * @gui: the front
 * @id:  the item: its range and its number
 */
static void range_used(struct gui *gui, ULONG id)
{
	ULONG number = id & ~(ULONG)MENU_RANGE;
	switch (id & MENU_RANGE)
	{
	case MENU_TRACK:
		if (number < gui->track_count)
			engine_select(gui->engine, gui->tracks[number].kind, gui->tracks[number].stream);
		break;
	case MENU_UNIT:
		gui->settings.audio_unit = number;
		engine_audio_unit(gui->engine, gui->settings.audio_unit, gui->settings.shared);
		break;
	case MENU_FILM:
		film_play(gui, number);
		break;
	case MENU_CHAPTER:
		if (number < gui->chapter_count)
			engine_seek(gui->engine, gui->chapters[number].start);
		break;
	case MENU_ASPECT:
		if (number < ARRAY_COUNT(aspects))
			aspect_set(gui, aspects[number].width, aspects[number].height);
		break;
	default:
		break;
	}
}

/**
 * play_used() - Act on an item of the Play and Playlist menus' fixed part.
 * @gui: the front
 * @id:  the item's menu_id
 *
 * Return: TRUE if the item was one of them.
 */
static BOOL play_used(struct gui *gui, ULONG id)
{
	switch (id)
	{
	case MENU_PLAY:
		play_toggle(gui);
		return TRUE;
	case MENU_STOP:
		film_stop(gui);
		return TRUE;
	case MENU_JUMP:
		jump_ask(gui);
		return TRUE;
	case MENU_NEXT:
		film_play(gui, gui->playlist.current + 1);
		return TRUE;
	case MENU_PREVIOUS:
		film_play(gui, gui->playlist.current - 1);
		return TRUE;
	default:
		return FALSE;
	}
}

/**
 * project_used() - Act on an item of the Project menu.
 * @gui: the front
 * @id:  the item's menu_id
 *
 * Return: TRUE if the item was one of them.
 */
static BOOL project_used(struct gui *gui, ULONG id)
{
	switch (id)
	{
	case MENU_OPEN:
	case MENU_ADD:
		films_ask(gui, id == MENU_ADD);
		return TRUE;
	case MENU_SUBTITLE_OPEN:
		subtitle_ask(gui);
		return TRUE;
	case MENU_CLOSE:
		film_close(gui);
		return TRUE;
	case MENU_QUIT:
		gui->quit = TRUE;
		return TRUE;
	default:
		return FALSE;
	}
}

/**
 * item_used() - Act on one menu item.
 * @gui: the front
 * @id:  the item's menu_id
 */
static void item_used(struct gui *gui, ULONG id)
{
	if (play_used(gui, id) || project_used(gui, id))
		return;
	switch (id)
	{
	case MENU_FONT:
		font_ask(gui);
		break;
	case MENU_FONT_OWN:
		font_set(gui, (CONST_STRPTR) "");
		break;
	case MENU_FULL_SCREEN:
		full_screen_toggle(gui);
		break;
	case MENU_STATISTICS:
		statistics_toggle(gui);
		break;
	default:
		break;
	}
}

/**
 * delay_used() - Act on one of the items that move the sound or the subtitles in time, or on No delays.
 * @gui: the front
 * @id:  the item's menu_id
 */
static void delay_used(struct gui *gui, ULONG id)
{
	LONG audio = gui->audio_delay, subtitles = gui->subtitle_delay;
	if (id == MENU_NO_DELAYS)
		audio = subtitles = 0;
	else if (id == MENU_AUDIO_LATER)
		audio += AUDIO_STEP;
	else if (id == MENU_AUDIO_EARLIER)
		audio -= AUDIO_STEP;
	else if (id == MENU_SUBTITLES_LATER)
		subtitles += SUBTITLE_STEP;
	else
		subtitles -= SUBTITLE_STEP;
	delays_set(gui, audio, subtitles);
}

/**
 * menu_used() - Act on the menu item picked, then make the menus again.
 * @gui:  the front
 * @code: the item's menu number
 *
 * Of several items picked in one go only the first counts: the menus the
 * others belong to are gone by then.
 */
void menu_used(struct gui *gui, UWORD code)
{
	struct MenuItem *item = gui->menus != NULL && code != MENUNULL ? ItemAddress(gui->menus, code) : NULL;
	if (item == NULL)
		return;
	ULONG id = (ULONG)GTMENUITEM_USERDATA(item);
	if (id >= MENU_TRACK)
		range_used(gui, id);
	else if (id == MENU_SHARED)
	{
		gui->settings.shared = !gui->settings.shared;
		engine_audio_unit(gui->engine, gui->settings.audio_unit, gui->settings.shared);
	}
	else if (id == MENU_SUBTITLE_OFF)
		engine_select(gui->engine, ENGINE_TRACK_SUBTITLE, ENGINE_TRACK_OFF);
	else if (id >= MENU_AUDIO_LATER && id <= MENU_NO_DELAYS)
		delay_used(gui, id);
	else
		item_used(gui, id);
	if (!gui->quit)
		menus_make(gui);
}
