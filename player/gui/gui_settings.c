/* SPDX-License-Identifier: MPL-2.0 */
/*
 * gui_settings.c - the settings window: what is set once and left.
 *
 *   Sound        the AHI unit, and whether to share it with other programs
 *   Subtitles    the font file, the size, the distance from the frame's edge
 *   Full screen  the Workbench's screen mode, or one chosen
 *
 * A change takes effect at once (the subtitles on a film that stands still
 * are drawn again with it).  Save keeps the settings for later runs; Cancel,
 * the close gadget and Esc put back the ones the window opened with.  Both
 * close the window.
 */
#include <string.h>
#include <intuition/gadgetclass.h>
#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/chooser.h>
#include <gadgets/checkbox.h>
#include <gadgets/getfile.h>
#include <gadgets/getscreenmode.h>
#include <gadgets/slider.h>
#include <gadgets/space.h>
#include <images/label.h>
#include <libraries/gadtools.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/window.h>
#include <proto/layout.h>
#include <proto/chooser.h>
#include <proto/checkbox.h>
#include <proto/getfile.h>
#include <proto/getscreenmode.h>
#include <proto/slider.h>
#include <proto/space.h>
#include <proto/label.h>
#include <reaction/reaction_macros.h>

#include "gui_private.h"

#define LEVEL_CHARACTERS 5 /* a slider's level as it shows: up to "200 %" */
#define RAWKEY_ESCAPE 0x45

struct Library *ChooserBase;
struct Library *CheckBoxBase;
struct Library *GetFileBase;
struct Library *GetScreenModeBase;
struct Library *LabelBase;

static STRPTR unit_labels[] = {(STRPTR) "Unit 0", (STRPTR) "Unit 1", (STRPTR) "Unit 2", (STRPTR) "Unit 3", NULL};

_Static_assert(sizeof(unit_labels) / sizeof(unit_labels[0]) == SETTINGS_UNITS + 1, "a label for each unit, then the end");

/**
 * mode_filter() - The screen mode requester's filter: only the modes the full screen can be in.
 * @hook:      the hook
 * @requester: the requester
 * @mode:      a mode's display ID
 *
 * Return: TRUE to list the mode.
 */
static ULONG mode_filter(struct Hook *hook asm("a0"), APTR requester asm("a2"), ULONG mode asm("a1"))
{
	(void)hook;
	(void)requester;
	return display_mode_usable(mode) ? 1u : 0u;
}

/**
 * label_make() - A gadget's label.
 * @text: its text
 *
 * Return: the label image, NULL without memory (the gadget then has none).
 */
static Object *label_make(CONST_STRPTR text)
{
	return NewObject(LABEL_GetClass(), NULL, LABEL_Text, (ULONG)text, TAG_END);
}

/**
 * group_make() - A box of gadgets with a title, one under the other.
 * @title: its title
 * @tags:  what goes into it: LAYOUT_AddChild and CHILD_Label pairs
 *
 * Return: the layout, NULL without memory.
 */
static Object *group_make(CONST_STRPTR title, const struct TagItem *tags)
{
	return NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_VERT, LAYOUT_BevelStyle, BVS_GROUP, LAYOUT_Label, (ULONG)title,
	                 LAYOUT_SpaceOuter, TRUE, TAG_MORE, (ULONG)tags);
}

/**
 * level_slider_make() - Make a slider that shows its level, in percent, at its right.
 * @id:    its number
 * @least: its lowest level
 * @most:  its highest level
 * @level: the level it starts at
 *
 * Return: the gadget, NULL without memory.
 */
static struct Gadget *level_slider_make(ULONG id, ULONG least, ULONG most, ULONG level)
{
	return (struct Gadget *)NewObject(SLIDER_GetClass(), NULL, GA_ID, id, GA_RelVerify, TRUE, SLIDER_Min, least, SLIDER_Max, most,
	                                  SLIDER_Level, level, SLIDER_Orientation, SLIDER_HORIZONTAL, SLIDER_LevelFormat, (ULONG) "%ld %%",
	                                  SLIDER_LevelPlace, PLACETEXT_RIGHT, SLIDER_LevelMaxLen, LEVEL_CHARACTERS, TAG_END);
}

/**
 * font_shown() - Show the subtitles' font file in its gadget.
 * @gui: the front, the window open
 */
static void font_shown(struct gui *gui)
{
	struct settings_window *window = &gui->settings_window;
	if (!font_file((CONST_STRPTR)gui->settings.font, window->font_path))
		window->font_path[0] = 0;
	gadget_set(window->gadgets[SETTING_FONT], window->window, GETFILE_FullFile, (ULONG)window->font_path);
}

/**
 * gadgets_make() - Make the window's gadgets, each showing its setting.
 * @gui: the front
 *
 * Return: TRUE, FALSE when one could not be made; those made are freed then.
 */
static BOOL gadgets_make(struct gui *gui)
{
	const struct settings *settings = &gui->settings;
	struct Gadget **gadgets = gui->settings_window.gadgets;
	char *font = gui->settings_window.font_path;
	if (!font_file((CONST_STRPTR)settings->font, font))
		font[0] = 0;
	BOOL as_workbench = settings->screen_mode == SETTINGS_WORKBENCH_MODE;
	gadgets[SETTING_UNIT] = (struct Gadget *)NewObject(CHOOSER_GetClass(), NULL, GA_ID, SETTING_UNIT, GA_RelVerify, TRUE, CHOOSER_PopUp, TRUE,
	                                                   CHOOSER_LabelArray, (ULONG)unit_labels, CHOOSER_Selected, settings->audio_unit, TAG_END);
	gadgets[SETTING_SHARED] = (struct Gadget *)NewObject(CHECKBOX_GetClass(), NULL, GA_ID, SETTING_SHARED, GA_RelVerify, TRUE,
	                                                     GA_Text, (ULONG) "Share the unit with other programs", GA_Selected, settings->shared,
	                                                     TAG_END);
	gadgets[SETTING_FONT] = (struct Gadget *)NewObject(GETFILE_GetClass(), NULL, GA_ID, SETTING_FONT, GA_RelVerify, TRUE,
	                                                   GETFILE_TitleText, (ULONG) "Subtitle font", GETFILE_FullFile, (ULONG)font,
	                                                   GETFILE_Pattern, (ULONG)FONT_PATTERN, GETFILE_DoPatterns, TRUE,
	                                                   GETFILE_RejectIcons, TRUE, GETFILE_ReadOnly, TRUE, TAG_END);
	gadgets[SETTING_FONT_OWN] = button_make(SETTING_FONT_OWN, (CONST_STRPTR) "Kinora's");
	gadgets[SETTING_SIZE] = level_slider_make(SETTING_SIZE, SUBTITLE_SIZE_LEAST, SUBTITLE_SIZE_MOST, settings->subtitle_size);
	gadgets[SETTING_DISTANCE] = level_slider_make(SETTING_DISTANCE, 0, SUBTITLE_DISTANCE_MOST, settings->subtitle_distance);
	gadgets[SETTING_AS_WORKBENCH] = (struct Gadget *)NewObject(CHECKBOX_GetClass(), NULL, GA_ID, SETTING_AS_WORKBENCH, GA_RelVerify, TRUE,
	                                                           GA_Text, (ULONG) "As the Workbench", GA_Selected, as_workbench, TAG_END);
	gadgets[SETTING_MODE] = (struct Gadget *)NewObject(GETSCREENMODE_GetClass(), NULL, GA_ID, SETTING_MODE, GA_RelVerify, TRUE,
	                                                   GA_Disabled, as_workbench, GETSCREENMODE_TitleText, (ULONG) "Full-screen mode",
	                                                   GETSCREENMODE_DisplayID, full_screen_mode(gui),
	                                                   GETSCREENMODE_FilterFunc, (ULONG)&gui->settings_window.mode_filter, TAG_END);
	gadgets[SETTING_SAVE] = button_make(SETTING_SAVE, (CONST_STRPTR) "Save");
	gadgets[SETTING_CANCEL] = button_make(SETTING_CANCEL, (CONST_STRPTR) "Cancel");
	for (ULONG id = SETTING_UNIT; id < SETTING_COUNT; id++)
		if (gadgets[id] == NULL)
		{
			for (ULONG made = SETTING_UNIT; made < SETTING_COUNT; made++)
				DisposeObject((Object *)gadgets[made]); /* NULL is passed over */
			return FALSE;
		}
	return TRUE;
}

/**
 * root_make() - Lay the window's gadgets out: the three groups, then Save and Cancel.
 * @gadgets: the gadgets, made
 *
 * Return: the layout, NULL without memory.
 */
static Object *root_make(struct Gadget **gadgets)
{
	const struct TagItem sound[] = {
	    {LAYOUT_AddChild, (ULONG)gadgets[SETTING_UNIT]}, {CHILD_Label, (ULONG)label_make((CONST_STRPTR) "AHI unit")},
	    {LAYOUT_AddChild, (ULONG)gadgets[SETTING_SHARED]}, {TAG_DONE, 0},
	};
	Object *font = NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ, LAYOUT_AddChild, (ULONG)gadgets[SETTING_FONT],
	                         LAYOUT_AddChild, (ULONG)gadgets[SETTING_FONT_OWN], CHILD_WeightedWidth, 0, TAG_END);
	const struct TagItem subtitles[] = {
	    {LAYOUT_AddChild, (ULONG)font}, {CHILD_Label, (ULONG)label_make((CONST_STRPTR) "Font")},
	    {LAYOUT_AddChild, (ULONG)gadgets[SETTING_SIZE]}, {CHILD_Label, (ULONG)label_make((CONST_STRPTR) "Size")},
	    {LAYOUT_AddChild, (ULONG)gadgets[SETTING_DISTANCE]}, {CHILD_Label, (ULONG)label_make((CONST_STRPTR) "Distance")},
	    {TAG_DONE, 0},
	};
	const struct TagItem screen[] = {
	    {LAYOUT_AddChild, (ULONG)gadgets[SETTING_AS_WORKBENCH]}, {CHILD_Label, (ULONG)label_make((CONST_STRPTR) "Screen mode")},
	    {LAYOUT_AddChild, (ULONG)gadgets[SETTING_MODE]}, {TAG_DONE, 0},
	};
	Object *buttons = NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
	                            LAYOUT_AddChild, (ULONG)gadgets[SETTING_SAVE], CHILD_WeightedWidth, 0,
	                            LAYOUT_AddChild, (ULONG)NewObject(SPACE_GetClass(), NULL, TAG_END),
	                            LAYOUT_AddChild, (ULONG)gadgets[SETTING_CANCEL], CHILD_WeightedWidth, 0, TAG_END);
	return NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_VERT, LAYOUT_SpaceOuter, TRUE,
	                 LAYOUT_AddChild, (ULONG)group_make((CONST_STRPTR) "Sound", sound), CHILD_WeightedHeight, 0,
	                 LAYOUT_AddChild, (ULONG)group_make((CONST_STRPTR) "Subtitles", subtitles), CHILD_WeightedHeight, 0,
	                 LAYOUT_AddChild, (ULONG)group_make((CONST_STRPTR) "Full screen", screen), CHILD_WeightedHeight, 0,
	                 LAYOUT_AddChild, (ULONG)buttons, CHILD_WeightedHeight, 0,
	                 TAG_END);
}

/**
 * settings_window_open() - Open the settings window, or bring it to the front.
 * @gui: the front
 *
 * It opens under the pointer, and keeps the settings as they are for Cancel.
 * Without memory the screen flashes.
 */
void settings_window_open(struct gui *gui)
{
	struct settings_window *window = &gui->settings_window;
	if (window->window != NULL)
	{
		WindowToFront(window->window);
		ActivateWindow(window->window);
		return;
	}
	window->before = gui->settings;
	window->mode_filter = (struct Hook){.h_Entry = (HOOKFUNC)(APTR)mode_filter};
	Object *root = gadgets_make(gui) ? root_make(window->gadgets) : NULL;
	if (root != NULL)
		window->window_object = NewObject(WINDOW_GetClass(), NULL,
		                                  WA_Title, (ULONG) "Kinora: Settings",
		                                  WA_DragBar, TRUE,
		                                  WA_DepthGadget, TRUE,
		                                  WA_CloseGadget, TRUE,
		                                  WA_Activate, TRUE,
		                                  WA_IDCMP, IDCMP_RAWKEY,
		                                  WINDOW_Position, WPOS_CENTERMOUSE,
		                                  WINDOW_ParentGroup, (ULONG)root,
		                                  TAG_END);
	if (window->window_object == NULL)
		DisposeObject(root); /* NULL is passed over */
	else
		window->window = (struct Window *)RA_OpenWindow(window->window_object);
	if (window->window == NULL)
	{
		settings_window_close(gui, TRUE); /* frees what was made */
		memset(window, 0, sizeof(*window));
		DisplayBeep(gui->player.window->WScreen);
	}
}

/**
 * settings_put_back() - Put back the settings the window opened with.
 * @gui:    the front
 * @before: those settings
 *
 * Only what the window sets goes back; the volume, the drawer and the recent
 * films stay as they are now.  While the player quits only the settings go
 * back, to be saved: the engine is about to end.
 */
static void settings_put_back(struct gui *gui, const struct settings *before)
{
	struct settings *now = &gui->settings;
	if (gui->quit)
	{
		now->audio_unit = before->audio_unit;
		now->shared = before->shared;
		strcpy(now->font, before->font);
		now->subtitle_size = before->subtitle_size;
		now->subtitle_distance = before->subtitle_distance;
		now->screen_mode = before->screen_mode;
		return;
	}
	if (before->audio_unit != now->audio_unit || before->shared != now->shared)
		unit_set(gui, before->audio_unit, before->shared);
	if (strcmp(before->font, now->font) != 0 && font_set(gui, (CONST_STRPTR)before->font) != 0)
		font_set(gui, (CONST_STRPTR) ""); /* the font is gone meanwhile: Kinora's own */
	if (before->subtitle_size != now->subtitle_size || before->subtitle_distance != now->subtitle_distance)
		look_set(gui, before->subtitle_size, before->subtitle_distance);
	gui->settings.screen_mode = before->screen_mode;
}

/**
 * settings_window_close() - Close the settings window.
 * @gui:  the front; nothing happens without the window
 * @keep: keep what was changed in it; else put back what it opened with
 */
void settings_window_close(struct gui *gui, BOOL keep)
{
	struct settings_window *window = &gui->settings_window;
	if (window->window_object == NULL)
		return;
	if (!keep)
		settings_put_back(gui, &window->before);
	DisposeObject(window->window_object); /* closes the window, frees the gadgets */
	memset(window, 0, sizeof(*window));
}

/**
 * settings_window_show() - Bring the window's gadgets up to the settings, changed from elsewhere (ARexx).
 * @gui: the front; nothing happens without the window
 */
void settings_window_show(struct gui *gui)
{
	struct settings_window *window = &gui->settings_window;
	if (window->window == NULL)
		return;
	const struct settings *settings = &gui->settings;
	struct Gadget **gadgets = window->gadgets;
	BOOL as_workbench = settings->screen_mode == SETTINGS_WORKBENCH_MODE;
	gadget_set(gadgets[SETTING_UNIT], window->window, CHOOSER_Selected, settings->audio_unit);
	gadget_set(gadgets[SETTING_SHARED], window->window, GA_Selected, (ULONG)(settings->shared != 0));
	font_shown(gui);
	gadget_set(gadgets[SETTING_SIZE], window->window, SLIDER_Level, settings->subtitle_size);
	gadget_set(gadgets[SETTING_DISTANCE], window->window, SLIDER_Level, settings->subtitle_distance);
	gadget_set(gadgets[SETTING_AS_WORKBENCH], window->window, GA_Selected, (ULONG)as_workbench);
	gadget_set(gadgets[SETTING_MODE], window->window, GETSCREENMODE_DisplayID, full_screen_mode(gui));
	gadget_set(gadgets[SETTING_MODE], window->window, GA_Disabled, (ULONG)as_workbench);
}

/**
 * font_chosen() - Ask for a font file with the font gadget's requester and draw the subtitles with it.
 * @gui: the front, the window open
 *
 * A file that is no font flashes the screen, and the gadget shows the font in use again.
 */
static void font_chosen(struct gui *gui)
{
	struct settings_window *window = &gui->settings_window;
	if (!gfRequestFile((Object *)window->gadgets[SETTING_FONT], window->window))
		return;
	STRPTR file = NULL;
	GetAttr(GETFILE_FullFile, (Object *)window->gadgets[SETTING_FONT], (ULONG *)&file);
	if (file == NULL || font_set(gui, (CONST_STRPTR)file) != 0)
	{
		DisplayBeep(window->window->WScreen);
		font_shown(gui);
	}
}

/**
 * mode_chosen() - Take the screen mode the mode gadget's requester gave, or the Workbench's.
 * @gui: the front, the window open
 * @ask: open the requester first; else only As the Workbench changed
 */
static void mode_chosen(struct gui *gui, BOOL ask)
{
	struct settings_window *window = &gui->settings_window;
	struct Gadget *mode_gadget = window->gadgets[SETTING_MODE];
	if (ask && !RequestScreenMode((Object *)mode_gadget, window->window))
		return;
	ULONG as_workbench = FALSE, mode = SETTINGS_WORKBENCH_MODE;
	GetAttr(GA_Selected, (Object *)window->gadgets[SETTING_AS_WORKBENCH], &as_workbench);
	if (!as_workbench)
		GetAttr(GETSCREENMODE_DisplayID, (Object *)mode_gadget, &mode);
	gui->settings.screen_mode = as_workbench || !display_mode_usable(mode) ? SETTINGS_WORKBENCH_MODE : mode;
	gadget_set(mode_gadget, window->window, GA_Disabled, as_workbench);
}

/**
 * look_used() - Act on the sound's and the subtitles' gadgets.
 * @gui:   the front, the window open
 * @id:    the gadget's number
 * @value: its value: the chooser's choice, the checkbox's state, the slider's level
 *
 * Return: TRUE if the gadget was one of them.
 */
static BOOL look_used(struct gui *gui, ULONG id, ULONG value)
{
	const struct settings *settings = &gui->settings;
	switch (id)
	{
	case SETTING_UNIT:
		unit_set(gui, value, settings->shared);
		return TRUE;
	case SETTING_SHARED:
		unit_set(gui, settings->audio_unit, value != 0);
		return TRUE;
	case SETTING_FONT:
		font_chosen(gui);
		return TRUE;
	case SETTING_FONT_OWN:
		font_set(gui, (CONST_STRPTR) "");
		font_shown(gui);
		return TRUE;
	case SETTING_SIZE:
		look_set(gui, value, settings->subtitle_distance);
		return TRUE;
	case SETTING_DISTANCE:
		look_set(gui, settings->subtitle_size, value);
		return TRUE;
	default:
		return FALSE;
	}
}

/**
 * setting_used() - Act on one of the window's gadgets.
 * @gui: the front, the window open
 * @id:  the gadget's number; its value is read from it
 */
static void setting_used(struct gui *gui, ULONG id)
{
	static const Tag value_tags[SETTING_COUNT] = {
	    [SETTING_UNIT] = CHOOSER_Selected, [SETTING_SHARED] = GA_Selected, [SETTING_SIZE] = SLIDER_Level, [SETTING_DISTANCE] = SLIDER_Level,
	};
	ULONG value = 0;
	if (id < SETTING_COUNT && value_tags[id] != 0)
		GetAttr(value_tags[id], (Object *)gui->settings_window.gadgets[id], &value);
	if (look_used(gui, id, value))
		return;
	if (id == SETTING_AS_WORKBENCH || id == SETTING_MODE)
		mode_chosen(gui, id == SETTING_MODE);
	else if (id == SETTING_SAVE)
	{
		settings_save(&gui->settings);
		settings_window_close(gui, TRUE);
	}
	else if (id == SETTING_CANCEL)
		settings_window_close(gui, FALSE);
}

/**
 * settings_window_input_take() - Take what the settings window has to say.
 * @gui: the front; nothing happens without the window
 *
 * A gadget may close the window: it is looked at anew for every message.
 */
void settings_window_input_take(struct gui *gui)
{
	struct settings_window *window = &gui->settings_window;
	ULONG result;
	UWORD code = 0;
	while (!gui->quit && window->window_object != NULL && (result = RA_HandleInput(window->window_object, &code)) != WMHI_LASTMSG)
	{
		switch (result & WMHI_CLASSMASK)
		{
		case WMHI_CLOSEWINDOW:
			settings_window_close(gui, FALSE);
			break;
		case WMHI_GADGETUP:
			setting_used(gui, result & WMHI_GADGETMASK);
			break;
		case WMHI_RAWKEY:
			if ((result & WMHI_KEYMASK) == RAWKEY_ESCAPE)
				settings_window_close(gui, FALSE);
			break;
		default:
			break;
		}
	}
}
