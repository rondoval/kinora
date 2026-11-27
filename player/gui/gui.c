/* SPDX-License-Identifier: MPL-2.0 */
/*
 * gui.c - the player's window, its gadgets and the event loop.
 *
 * A ReAction window: a space gadget the engine draws the pictures into, the
 * seek slider under it, then a row with the buttons, the volume and a status
 * line.  The same row and slider, without the picture, make the strip shown
 * over a full screen's foot (gui_screen.c).
 *
 * The engine draws into the window from its own task: the place is taken
 * away from it before the window closes.
 */
#include <stdio.h>
#include <string.h>
#include <dos/dos.h>
#include <intuition/gadgetclass.h>
#include <workbench/startup.h>
#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/space.h>
#include <gadgets/button.h>
#include <gadgets/slider.h>
#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/asl.h>
#include <proto/gadtools.h>
#include <proto/window.h>
#include <proto/layout.h>
#include <proto/space.h>
#include <proto/button.h>
#include <proto/slider.h>
#include <reaction/reaction_macros.h>

#include "gui_private.h"
#include "../audio/audio_out.h"

#define PICTURE_MIN_WIDTH 320      /* the smallest picture box */
#define PICTURE_MIN_HEIGHT 180
#define PICTURE_WIDTH 960          /* the box the window opens with */
#define PICTURE_HEIGHT 540
#define TICK_MICROSECONDS 250000   /* how often the status line and the slider follow the film */
#define MICROSECONDS_PER_SECOND 1000000
#define MICROSECONDS_PER_MS 1000
#define SECONDS_PER_MINUTE 60
#define KEY_STEP (10 * (VCTime)MICROSECONDS_PER_SECOND) /* the cursor keys move by this much */

/* What the file requesters show at first; their pattern gadget shows others */
/* demuxer.library's containers: Matroska, MP4/QuickTime, MPEG-TS, AVI, MPEG-PS, and MPEG-1/2 video alone */
#define FILM_PATTERN "#?.(mkv|webm|mp4|m4v|mov|ts|m2ts|mts|avi|divx|mpg|mpeg|vob|m1v|m2v)"
#define SUBTITLE_PATTERN "#?.srt"                             /* SubRip */
#define FONT_PATTERN "#?.(ttf|otf)"                           /* what FreeType reads */

#define RAWKEY_I 0x17
#define RAWKEY_A 0x20
#define RAWKEY_S 0x21
#define RAWKEY_F 0x23
#define RAWKEY_SPACE 0x40
#define RAWKEY_ESCAPE 0x45
#define RAWKEY_PAGE_UP 0x48
#define RAWKEY_PAGE_DOWN 0x49
#define RAWKEY_RIGHT 0x4E
#define RAWKEY_LEFT 0x4F

struct Library *WindowBase;
struct Library *LayoutBase;
struct Library *SpaceBase;
struct Library *ButtonBase;
struct Library *SliderBase;

static const char *const state_names[] = {"No film", "Paused", "Playing", "Ended"};

/**
 * tick_send() - Send the next tick.
 * @gui: the front
 */
static void tick_send(struct gui *gui)
{
	gui->timer->tr_node.io_Command = TR_ADDREQUEST;
	gui->timer->tr_time.tv_secs = 0;
	gui->timer->tr_time.tv_micro = TICK_MICROSECONDS;
	SendIO(&gui->timer->tr_node);
	gui->timer_sent = TRUE;
}

/**
 * picture_box() - Where the picture box is in the window now.
 * @gui:   the front, its window open
 * @place: filled in
 */
void picture_box(struct gui *gui, struct display_place *place)
{
	struct IBox *box = NULL;
	GetAttr(SPACE_AreaBox, (Object *)gui->player.gadgets[GID_PICTURE], (ULONG *)&box);
	*place = (struct display_place){.window = gui->player.window};
	if (box != NULL)
	{
		place->left = box->Left;
		place->top = box->Top;
		place->width = box->Width;
		place->height = box->Height;
	}
}

/**
 * controls_update() - Bring one window's status line and seek slider up to the film.
 * @controls: the window; one that is not open is left alone
 * @text:     the status line
 * @level:    the seek slider's level, -1 to leave it
 *
 * The slider is left alone while the user holds it.
 */
static void controls_update(struct controls *controls, const char *text, LONG level)
{
	if (controls->window == NULL)
		return;
	if (strcmp(text, controls->status) != 0)
	{
		strcpy(controls->status, text);
		SetGadgetAttrs(controls->gadgets[GID_STATUS], controls->window, NULL, GA_Text, (ULONG)controls->status, TAG_DONE);
	}
	if (level >= 0 && (controls->gadgets[GID_SEEK]->Flags & GFLG_SELECTED) == 0)
		SetGadgetAttrs(controls->gadgets[GID_SEEK], controls->window, NULL, SLIDER_Level, (ULONG)level, TAG_DONE);
}

/**
 * film_follow() - Follow the film: bring the status lines and seek sliders up to it, and go on to the next film when one ends.
 * @gui: the front
 */
void film_follow(struct gui *gui)
{
	struct engine_status status;
	engine_status(gui->engine, &status);
	if (status.state == ENGINE_ENDED && gui->playlist.current + 1 < gui->playlist.count)
	{
		film_play(gui, gui->playlist.current + 1);
		return;
	}
	BOOL has_length = status.duration != VC_TIME_UNKNOWN && status.duration > 0;
	long position = (long)(status.position / MICROSECONDS_PER_SECOND);
	long length = has_length ? (long)(status.duration / MICROSECONDS_PER_SECOND) : 0;
	char text[STATUS_SIZE];
	snprintf(text, sizeof(text), "%ld:%02ld / %ld:%02ld  %s%s  %lux%lu  %lu dropped", position / SECONDS_PER_MINUTE,
	         position % SECONDS_PER_MINUTE, length / SECONDS_PER_MINUTE, length % SECONDS_PER_MINUTE, state_names[status.state],
	         status.error != 0 ? " (error)" : "", (unsigned long)status.width, (unsigned long)status.height,
	         (unsigned long)status.frames_dropped);
	VCTime level = has_length ? status.position * SEEK_STEPS / status.duration : -1;
	controls_update(&gui->player, text, (LONG)level);
	controls_update(&gui->strip, text, (LONG)level);
}

/**
 * place_now() - Where the picture goes at the moment.
 * @gui:   the front
 * @place: filled in: the full screen while there is one, else the window's box
 */
static void place_now(struct gui *gui, struct display_place *place)
{
	if (gui->screen == NULL)
		picture_box(gui, place);
	else
		full_screen_place(gui, place);
}

/**
 * film_play() - Open one of the playlist's films and start it.
 * @gui:   the front
 * @index: the film; one past the playlist's end does nothing
 */
void film_play(struct gui *gui, ULONG index)
{
	if (index >= gui->playlist.count)
		return;
	struct display_place place;
	place_now(gui, &place);
	gui->playlist.current = index;
	if (engine_open(gui->engine, (CONST_STRPTR)gui->playlist.films[index], &place) == 0)
		engine_play(gui->engine, TRUE);
	else
		DisplayBeep(gui->player.window->WScreen);
	menus_make(gui);
}

/**
 * film_close() - Close the film: the player stands empty, its picture black, the full screen left; the playlist stays.
 * @gui: the front
 */
void film_close(struct gui *gui)
{
	engine_close(gui->engine);
	if (gui->screen != NULL)
		full_screen_toggle(gui);
	menus_make(gui);
	film_follow(gui);
}

/**
 * film_add() - Put a film behind the playlist's films.
 * @gui:  the front
 * @name: its path
 * @play: and play it now
 */
void film_add(struct gui *gui, CONST_STRPTR name, BOOL play)
{
	if (!playlist_add(&gui->playlist, name))
		return;
	if (play)
		film_play(gui, gui->playlist.count - 1);
	else
		menus_make(gui);
}

/**
 * aspect_set() - Show the picture in another shape.
 * @gui:    the front
 * @width:  the shape's width to its @height; 0 for the film's own; DISPLAY_ASPECT_FILL to stretch it
 * @height: see @width
 */
void aspect_set(struct gui *gui, LONG width, LONG height)
{
	gui->aspect_width = width;
	gui->aspect_height = height;
	engine_aspect(gui->engine, width, height);
}

/**
 * delays_set() - Put the sound and the subtitles behind the picture, or ahead of it.
 * @gui:       the front
 * @audio:     milliseconds the sound comes later
 * @subtitles: milliseconds the subtitles come later
 */
void delays_set(struct gui *gui, LONG audio, LONG subtitles)
{
	gui->audio_delay = audio;
	gui->subtitle_delay = subtitles;
	engine_delays(gui->engine, (VCTime)audio * MICROSECONDS_PER_MS, (VCTime)subtitles * MICROSECONDS_PER_MS);
}

/**
 * film_stop() - Stand still at the film's beginning.
 * @gui: the front
 */
void film_stop(struct gui *gui)
{
	engine_play(gui->engine, FALSE);
	engine_seek(gui->engine, 0);
}

/**
 * requester_path() - The whole path of one file a requester returned.
 * @drawer_lock: a lock on the file's drawer, as a multi-select gives each file (wa_Lock); 0 for none
 * @drawer_name: the drawer's name as the requester holds it (fr_Drawer), taken when @drawer_lock is 0
 * @name:        the file's name in that drawer
 * @path:        gets the path, PATH_SIZE bytes
 *
 * Return: TRUE, FALSE when the path does not fit.
 */
static BOOL requester_path(BPTR drawer_lock, CONST_STRPTR drawer_name, CONST_STRPTR name, char *path)
{
	if (drawer_lock != 0)
	{
		if (!NameFromLock(drawer_lock, (STRPTR)path, PATH_SIZE))
			return FALSE;
	}
	else
	{
		if (strlen((const char *)drawer_name) >= PATH_SIZE)
			return FALSE;
		strcpy(path, (const char *)drawer_name);
	}
	return AddPart((STRPTR)path, name, PATH_SIZE) != 0;
}

/**
 * requester_films() - Put the films chosen in the file requester behind the playlist's.
 * @gui:       the front
 * @requester: the requester, answered
 */
static void requester_films(struct gui *gui, const struct FileRequester *requester)
{
	char path[PATH_SIZE];
	for (LONG i = 0; i < requester->fr_NumArgs; i++)
		if (requester_path(requester->fr_ArgList[i].wa_Lock, requester->fr_Drawer, (CONST_STRPTR)requester->fr_ArgList[i].wa_Name, path))
			playlist_add(&gui->playlist, (CONST_STRPTR)path);
	if (requester->fr_NumArgs == 0 && requester_path(0, requester->fr_Drawer, requester->fr_File, path))
		playlist_add(&gui->playlist, (CONST_STRPTR)path);
}

/**
 * films_ask() - Ask for films with the file requester and play them.
 * @gui: the front
 * @add: put them behind the playlist's films; else in their place, and play the first
 */
void films_ask(struct gui *gui, BOOL add)
{
	if (gui->film_requester == NULL)
		gui->film_requester = AllocAslRequestTags(ASL_FileRequest,
		                                          ASLFR_TitleText, (ULONG) "Open films",
		                                          ASLFR_RejectIcons, TRUE,
		                                          ASLFR_DoMultiSelect, TRUE,
		                                          ASLFR_DoPatterns, TRUE,
		                                          ASLFR_InitialPattern, (ULONG)FILM_PATTERN,
		                                          ASLFR_InitialDrawer, (ULONG)gui->settings.drawer,
		                                          TAG_DONE);
	struct FileRequester *requester = gui->film_requester;
	if (requester == NULL || !AslRequestTags(requester, ASLFR_Window, (ULONG)gui->player.window, ASLFR_SleepWindow, TRUE, TAG_DONE))
		return;
	if (strlen((const char *)requester->fr_Drawer) < sizeof(gui->settings.drawer))
		strcpy(gui->settings.drawer, (const char *)requester->fr_Drawer);
	if (!add)
		playlist_clear(&gui->playlist);
	requester_films(gui, requester);
	if (!add)
		film_play(gui, 0);
}

/**
 * subtitle_ask() - Ask for a SubRip file with the file requester and show its subtitles.
 * @gui: the front
 */
void subtitle_ask(struct gui *gui)
{
	if (gui->subtitle_requester == NULL)
		gui->subtitle_requester =
		    AllocAslRequestTags(ASL_FileRequest,
		                        ASLFR_TitleText, (ULONG) "Open subtitles",
		                        ASLFR_RejectIcons, TRUE,
		                        ASLFR_DoPatterns, TRUE,
		                        ASLFR_InitialPattern, (ULONG)SUBTITLE_PATTERN,
		                        ASLFR_InitialDrawer, (ULONG)gui->settings.drawer,
		                        TAG_DONE);
	struct FileRequester *requester = gui->subtitle_requester;
	if (requester == NULL || !AslRequestTags(requester, ASLFR_Window, (ULONG)gui->player.window, ASLFR_SleepWindow, TRUE, TAG_DONE))
		return;
	char path[PATH_SIZE];
	if (!requester_path(0, requester->fr_Drawer, requester->fr_File, path) || engine_subtitle_file(gui->engine, (CONST_STRPTR)path) != 0)
		DisplayBeep(gui->player.window->WScreen);
}

/**
 * click_doubled() - Note a click on the picture, and say whether it is the second of a double click.
 * @gui: the front
 *
 * Return: TRUE for the second click; a third one starts anew.
 */
BOOL click_doubled(struct gui *gui)
{
	ULONG seconds = 0, micros = 0;
	CurrentTime(&seconds, &micros);
	BOOL second = gui->click_seconds != 0 && DoubleClick(gui->click_seconds, gui->click_micros, seconds, micros);
	gui->click_seconds = second ? 0 : seconds;
	gui->click_micros = second ? 0 : micros;
	return second;
}

/**
 * picture_clicked() - A click in the player's window: on the picture it plays or pauses; a double click goes to the full screen.
 * @gui:  the front, its window open
 * @code: the button's code; only the left button going down counts
 *
 * The two clicks of a double click play and pause, or pause and play: the film is left as it was.
 */
static void picture_clicked(struct gui *gui, UWORD code)
{
	struct display_place place;
	picture_box(gui, &place);
	LONG x = gui->player.window->MouseX, y = gui->player.window->MouseY;
	if (code != SELECTDOWN || x < place.left || x >= place.left + place.width || y < place.top || y >= place.top + place.height)
		return;
	play_toggle(gui);
	if (click_doubled(gui))
		full_screen_toggle(gui);
}

/**
 * statistics_toggle() - Show the statistics panel in the picture, or take it away.
 * @gui: the front
 */
void statistics_toggle(struct gui *gui)
{
	gui->statistics_on = !gui->statistics_on;
	engine_statistics(gui->engine, gui->statistics_on, NULL, 0);
}

/**
 * font_set() - Draw the subtitles with a font, and remember it.
 * @gui:  the front
 * @name: the font file; an empty name is Kinora's own font, in the drawer Fonts beside the program
 *
 * Return: 0, the engine's error when the file is no font; the font before then stays.
 */
LONG font_set(struct gui *gui, CONST_STRPTR name)
{
	char own[PATH_SIZE];
	CONST_STRPTR file = name;
	if (name[0] == 0)
	{
		if (!NameFromLock(GetProgramDir(), (STRPTR)own, PATH_SIZE) || !AddPart((STRPTR)own, (CONST_STRPTR)OWN_FONT, PATH_SIZE))
			return ENGINE_ERR_FILE;
		file = (CONST_STRPTR)own;
	}
	if (strlen((const char *)name) >= sizeof(gui->settings.font))
		return ENGINE_ERR_FILE;
	LONG error = engine_subtitle_font(gui->engine, file);
	if (error == 0)
		strcpy(gui->settings.font, (const char *)name);
	return error;
}

/**
 * font_ask() - Ask for a font file with the file requester and draw the subtitles with it.
 * @gui: the front
 */
void font_ask(struct gui *gui)
{
	if (gui->font_requester == NULL)
		gui->font_requester = AllocAslRequestTags(ASL_FileRequest,
		                                          ASLFR_TitleText, (ULONG) "Subtitle font",
		                                          ASLFR_RejectIcons, TRUE,
		                                          ASLFR_DoPatterns, TRUE,
		                                          ASLFR_InitialPattern, (ULONG)FONT_PATTERN,
		                                          TAG_DONE);
	struct FileRequester *requester = gui->font_requester;
	if (requester == NULL || !AslRequestTags(requester, ASLFR_Window, (ULONG)gui->player.window, ASLFR_SleepWindow, TRUE, TAG_DONE))
		return;
	char path[PATH_SIZE];
	if (!requester_path(0, requester->fr_Drawer, requester->fr_File, path) || font_set(gui, (CONST_STRPTR)path) != 0)
		DisplayBeep(gui->player.window->WScreen);
}

/**
 * play_toggle() - Play, pause, or start an ended film again; with none open, the playlist's film.
 * @gui: the front
 */
void play_toggle(struct gui *gui)
{
	struct engine_status status;
	engine_status(gui->engine, &status);
	if (status.state == ENGINE_EMPTY)
	{
		film_play(gui, gui->playlist.current); /* closed, or it did not open: open it (again) */
		return;
	}
	if (status.state == ENGINE_ENDED)
		engine_seek(gui->engine, 0);
	engine_play(gui->engine, status.state != ENGINE_PLAYING);
}

/**
 * seek_by() - Move in the film by a time.
 * @gui:  the front
 * @step: microseconds, negative for back
 */
static void seek_by(struct gui *gui, VCTime step)
{
	struct engine_status status;
	engine_status(gui->engine, &status);
	VCTime target = status.position + step;
	engine_seek(gui->engine, target < 0 ? 0 : target);
}

/**
 * seek_to_level() - Move in the film to where a seek slider was put.
 * @gui:   the front
 * @level: the slider's level, 0 to SEEK_STEPS
 */
static void seek_to_level(struct gui *gui, ULONG level)
{
	struct engine_status status;
	engine_status(gui->engine, &status);
	if (status.duration != VC_TIME_UNKNOWN)
		engine_seek(gui->engine, status.duration * (VCTime)level / SEEK_STEPS);
}

/**
 * gadget_used() - Act on a gadget.
 * @gui:  the front
 * @id:   the gadget's number
 * @code: its value: a slider's level
 */
static void gadget_used(struct gui *gui, ULONG id, ULONG code)
{
	gui->strip_idle_ticks = 0;
	switch (id)
	{
	case GID_PLAY:
		play_toggle(gui);
		break;
	case GID_STOP:
		film_stop(gui);
		break;
	case GID_PREVIOUS:
		film_play(gui, gui->playlist.current - 1);
		break;
	case GID_NEXT:
		film_play(gui, gui->playlist.current + 1);
		break;
	case GID_SEEK:
		seek_to_level(gui, code);
		break;
	case GID_VOLUME:
		gui->settings.volume = code;
		engine_volume(gui->engine, code * AUDIO_VOLUME_FULL / VOLUME_STEPS);
		break;
	default:
		break;
	}
}

/**
 * track_cycle() - Go on to the film's next track of a kind.
 * @gui:  the front
 * @kind: audio or subtitle
 *
 * Behind the last subtitle track comes "off"; behind the last audio track the first.
 */
static void track_cycle(struct gui *gui, enum engine_track_kind kind)
{
	LONG first = ENGINE_TRACK_OFF, next = ENGINE_TRACK_OFF;
	BOOL passed = FALSE;
	gui->track_count = engine_tracks(gui->engine, gui->tracks, ENGINE_TRACKS);
	for (ULONG i = 0; i < gui->track_count; i++)
	{
		const struct engine_track *track = &gui->tracks[i];
		if (track->kind != kind)
			continue;
		if (first == ENGINE_TRACK_OFF)
			first = track->stream;
		if (passed && next == ENGINE_TRACK_OFF)
			next = track->stream;
		passed = passed || track->selected;
	}
	if (next == ENGINE_TRACK_OFF && (kind == ENGINE_TRACK_AUDIO || !passed))
		next = first;
	engine_select(gui->engine, kind, next);
	menus_make(gui);
}

/**
 * key_moves() - Act on a key that moves in the film or the playlist.
 * @gui:  the front
 * @code: the raw key
 *
 * Return: TRUE if the key was one of them.
 */
static BOOL key_moves(struct gui *gui, ULONG code)
{
	switch (code)
	{
	case RAWKEY_SPACE:
		play_toggle(gui);
		return TRUE;
	case RAWKEY_LEFT:
		seek_by(gui, -KEY_STEP);
		return TRUE;
	case RAWKEY_RIGHT:
		seek_by(gui, KEY_STEP);
		return TRUE;
	case RAWKEY_PAGE_UP:
		film_play(gui, gui->playlist.current - 1);
		return TRUE;
	case RAWKEY_PAGE_DOWN:
		film_play(gui, gui->playlist.current + 1);
		return TRUE;
	default:
		return FALSE;
	}
}

/**
 * key_used() - Act on a key.
 * @gui:  the front
 * @code: the raw key; a key going up does nothing
 */
void key_used(struct gui *gui, ULONG code)
{
	if (key_moves(gui, code))
		return;
	switch (code)
	{
	case RAWKEY_F:
		full_screen_toggle(gui);
		break;
	case RAWKEY_A:
		track_cycle(gui, ENGINE_TRACK_AUDIO);
		break;
	case RAWKEY_I:
		statistics_toggle(gui);
		break;
	case RAWKEY_S:
		track_cycle(gui, ENGINE_TRACK_SUBTITLE);
		break;
	case RAWKEY_ESCAPE:
		if (gui->screen != NULL)
			full_screen_toggle(gui);
		else
			gui->quit = TRUE;
		break;
	default:
		break;
	}
}

/**
 * window_resized() - The player's window has another size: tell the engine where the picture box is.
 * @gui: the front
 */
static void window_resized(struct gui *gui)
{
	struct display_place place;
	if (gui->screen != NULL)
		return; /* the picture is on the full screen */
	picture_box(gui, &place);
	engine_place(gui->engine, &place);
}

/**
 * controls_input_take() - Take what one of the windows has to say.
 * @gui:      the front
 * @controls: the window; one that is not open says nothing
 *
 * A key or gadget may close the window asked: it is looked at anew for every message.
 */
void controls_input_take(struct gui *gui, struct controls *controls)
{
	ULONG result;
	UWORD code = 0;
	while (!gui->quit && controls->window_object != NULL && (result = RA_HandleInput(controls->window_object, &code)) != WMHI_LASTMSG)
	{
		switch (result & WMHI_CLASSMASK)
		{
		case WMHI_CLOSEWINDOW:
			gui->quit = TRUE;
			break;
		case WMHI_GADGETUP:
			gadget_used(gui, result & WMHI_GADGETMASK, code);
			break;
		case WMHI_MENUPICK:
			menu_used(gui, (UWORD)(result & WMHI_MENUMASK));
			break;
		case WMHI_RAWKEY:
			key_used(gui, result & WMHI_KEYMASK);
			break;
		case WMHI_NEWSIZE:
			window_resized(gui);
			break;
		case WMHI_MOUSEBUTTONS:
			picture_clicked(gui, code); /* only the player's window asks for the buttons */
			break;
		default:
			break;
		}
	}
}

/**
 * events_loop() - Serve the windows until the player is asked to quit.
 * @gui: the front, its window open
 */
static void events_loop(struct gui *gui)
{
	ULONG tick_signal = 1UL << gui->timer_port->mp_SigBit;
	tick_send(gui);
	while (!gui->quit)
	{
		ULONG window_signals = 0;
		GetAttr(WINDOW_SigMask, gui->player.window_object, &window_signals);
		ULONG ports = (gui->rexx_port != NULL ? 1UL << gui->rexx_port->mp_SigBit : 0) |
		              (gui->drop_port != NULL ? 1UL << gui->drop_port->mp_SigBit : 0);
		ULONG got = Wait(window_signals | full_screen_signals(gui) | tick_signal | ports | 1UL << gui->engine_signal | SIGBREAKF_CTRL_C);
		if (got & SIGBREAKF_CTRL_C)
			gui->quit = TRUE;
		if ((got & tick_signal) != 0 && CheckIO(&gui->timer->tr_node) != NULL)
		{
			WaitIO(&gui->timer->tr_node);
			tick_send(gui);
			full_screen_tick(gui);
		}
		controls_input_take(gui, &gui->player);
		full_screen_input_take(gui);
		rexx_take(gui);
		drop_take(gui);
		if (!gui->quit)
			film_follow(gui);
	}
}

/**
 * button_make() - Make a button.
 * @id:   its number
 * @text: its label
 *
 * Return: the gadget, NULL without memory.
 */
static struct Gadget *button_make(ULONG id, CONST_STRPTR text)
{
	return (struct Gadget *)NewObject(BUTTON_GetClass(), NULL, GA_ID, id, GA_RelVerify, TRUE, GA_Text, (ULONG)text, TAG_END);
}

/**
 * slider_make() - Make a horizontal slider.
 * @id:    its number
 * @most:  its highest level
 * @level: the level it starts at
 *
 * Return: the gadget, NULL without memory.
 */
static struct Gadget *slider_make(ULONG id, ULONG most, ULONG level)
{
	return (struct Gadget *)NewObject(SLIDER_GetClass(), NULL, GA_ID, id, GA_RelVerify, TRUE, SLIDER_Min, 0, SLIDER_Max, most, SLIDER_Level,
	                                  level, SLIDER_Orientation, SLIDER_HORIZONTAL, TAG_END);
}

/**
 * gadgets_make() - Make a window's gadgets.
 * @controls: the window
 * @picture:  with the picture box
 * @volume:   where the volume slider stands, 0 to VOLUME_STEPS
 *
 * Return: TRUE, FALSE when one could not be made.
 */
static BOOL gadgets_make(struct controls *controls, BOOL picture, ULONG volume)
{
	struct Gadget **gadgets = controls->gadgets;
	if (picture)
		gadgets[GID_PICTURE] = (struct Gadget *)NewObject(SPACE_GetClass(), NULL, GA_ID, GID_PICTURE, SPACE_MinWidth, PICTURE_MIN_WIDTH,
		                                                  SPACE_MinHeight, PICTURE_MIN_HEIGHT, TAG_END);
	gadgets[GID_SEEK] = slider_make(GID_SEEK, SEEK_STEPS, 0);
	gadgets[GID_PREVIOUS] = button_make(GID_PREVIOUS, (CONST_STRPTR) "|<");
	gadgets[GID_PLAY] = button_make(GID_PLAY, (CONST_STRPTR) "Play / Pause");
	gadgets[GID_STOP] = button_make(GID_STOP, (CONST_STRPTR) "Stop");
	gadgets[GID_NEXT] = button_make(GID_NEXT, (CONST_STRPTR) ">|");
	gadgets[GID_VOLUME] = slider_make(GID_VOLUME, VOLUME_STEPS, volume);
	gadgets[GID_STATUS] = (struct Gadget *)NewObject(BUTTON_GetClass(), NULL, GA_ID, GID_STATUS, GA_ReadOnly, TRUE, GA_Text,
	                                                 (ULONG) "No film", BUTTON_Justification, BCJ_LEFT, TAG_END);
	for (ULONG id = picture ? GID_PICTURE : GID_SEEK; id < GID_COUNT; id++)
		if (gadgets[id] == NULL)
			return FALSE; /* the ones made are lost: the program ends */
	return TRUE;
}

/**
 * controls_make() - Make and open a window with the controls.
 * @controls: the window to make
 * @screen:   NULL for the player's window, with the picture box above the controls; else the full screen, for the
 *            borderless strip of controls along its foot
 * @volume:   where the volume slider stands, 0 to VOLUME_STEPS
 *
 * Return: TRUE with the window open, FALSE when it could not be made or opened.
 */
BOOL controls_make(struct controls *controls, struct Screen *screen, ULONG volume)
{
	struct Gadget **gadgets = controls->gadgets;
	controls->status[0] = 0;
	if (!gadgets_make(controls, screen == NULL, volume))
		return FALSE;
	Object *row = NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ, LAYOUT_AddChild, (ULONG)gadgets[GID_PREVIOUS],
	                        CHILD_WeightedWidth, 0, LAYOUT_AddChild, (ULONG)gadgets[GID_PLAY], CHILD_WeightedWidth, 0, LAYOUT_AddChild,
	                        (ULONG)gadgets[GID_STOP], CHILD_WeightedWidth, 0, LAYOUT_AddChild, (ULONG)gadgets[GID_NEXT], CHILD_WeightedWidth,
	                        0, LAYOUT_AddChild, (ULONG)gadgets[GID_VOLUME], CHILD_WeightedWidth, 25, LAYOUT_AddChild,
	                        (ULONG)gadgets[GID_STATUS], TAG_END);
	Object *root = NewObject(LAYOUT_GetClass(), NULL,
	                         LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
	                         LAYOUT_SpaceOuter, TRUE,
	                         screen == NULL ? LAYOUT_AddChild : TAG_IGNORE, (ULONG)gadgets[GID_PICTURE],
	                         LAYOUT_AddChild, (ULONG)gadgets[GID_SEEK],
	                         CHILD_WeightedHeight, 0,
	                         LAYOUT_AddChild, (ULONG)row,
	                         CHILD_WeightedHeight, 0,
	                         TAG_END);
	if (screen == NULL)
		controls->window_object = NewObject(WINDOW_GetClass(), NULL,
		                                    WA_Title, (ULONG) "Kinora",
		                                    WA_DragBar, TRUE,
		                                    WA_DepthGadget, TRUE,
		                                    WA_CloseGadget, TRUE,
		                                    WA_SizeGadget, TRUE,
		                                    WA_Activate, TRUE,
		                                    WA_NewLookMenus, TRUE,
		                                    WA_InnerWidth, PICTURE_WIDTH,
		                                    WA_InnerHeight, PICTURE_HEIGHT,
		                                    WA_IDCMP, IDCMP_RAWKEY | IDCMP_MENUPICK | IDCMP_NEWSIZE | IDCMP_MOUSEBUTTONS,
		                                    WINDOW_Position, WPOS_CENTERSCREEN,
		                                    WINDOW_ParentGroup, (ULONG)root,
		                                    TAG_END);
	else
		controls->window_object = NewObject(WINDOW_GetClass(), NULL,
		                                    WA_CustomScreen, (ULONG)screen,
		                                    WA_Borderless, TRUE,
		                                    WA_Left, 0,
		                                    WA_Width, screen->Width,
		                                    WA_Activate, FALSE,
		                                    WA_IDCMP, IDCMP_RAWKEY,
		                                    WINDOW_ParentGroup, (ULONG)root,
		                                    TAG_END);
	if (controls->window_object != NULL)
		controls->window = (struct Window *)RA_OpenWindow(controls->window_object);
	return controls->window != NULL;
}

/**
 * controls_free() - Close a window with the controls and free its gadgets.
 * @controls: the window; one that was not made is left alone
 */
void controls_free(struct controls *controls)
{
	if (controls->window_object != NULL)
		DisposeObject(controls->window_object); /* closes the window, frees the gadgets */
	*controls = (struct controls){0};
}

/**
 * timer_make() - Open timer.device for the ticks.
 * @gui: the front
 *
 * Return: TRUE, FALSE when the port, the request or the device could not be had.
 */
static BOOL timer_make(struct gui *gui)
{
	gui->timer_port = CreateMsgPort();
	if (gui->timer_port == NULL)
		return FALSE;
	gui->timer = (struct timerequest *)CreateIORequest(gui->timer_port, sizeof(*gui->timer));
	gui->timer_open = gui->timer != NULL && OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, &gui->timer->tr_node, 0) == 0;
	return gui->timer_open;
}

/**
 * timer_free() - Stop the tick and close timer.device, as far as timer_make() got.
 * @gui: the front
 */
static void timer_free(struct gui *gui)
{
	if (gui->timer_sent)
	{
		AbortIO(&gui->timer->tr_node);
		WaitIO(&gui->timer->tr_node);
	}
	if (gui->timer_open)
		CloseDevice(&gui->timer->tr_node);
	if (gui->timer != NULL)
		DeleteIORequest(&gui->timer->tr_node);
	if (gui->timer_port != NULL)
		DeleteMsgPort(gui->timer_port);
}

/**
 * gui_serve() - Start the engine, open the window and serve it.
 * @gui:   the front, its signal and timer made, its settings read
 * @setup: what the player was started with
 *
 * Return: RETURN_OK, RETURN_FAIL when the engine or the window did not come up.
 */
static int gui_serve(struct gui *gui, const struct gui_setup *setup)
{
	struct engine_setup engine_setup = {
	    .listener = FindTask(NULL),
	    .signal = 1UL << gui->engine_signal,
	    .audio_unit = gui->settings.audio_unit,
	    .audio_shared = gui->settings.shared,
	};
	gui->engine = engine_start(&engine_setup);
	if (gui->engine == NULL || !controls_make(&gui->player, NULL, gui->settings.volume))
		return RETURN_FAIL;

	engine_volume(gui->engine, gui->settings.volume * AUDIO_VOLUME_FULL / VOLUME_STEPS);
	if (font_set(gui, (CONST_STRPTR)gui->settings.font) != 0)
		font_set(gui, (CONST_STRPTR) ""); /* the font chosen is gone: Kinora's own again */
	for (CONST_STRPTR *file = setup->files; file != NULL && *file != NULL; file++)
		playlist_add(&gui->playlist, *file);

	rexx_open(gui);
	drop_open(gui);
	menus_make(gui);

	film_play(gui, 0);
	events_loop(gui);

	drop_close(gui);
	rexx_close(gui);
	engine_place(gui->engine, NULL); /* before the window and the screen go: the engine draws into them */
	full_screen_close(gui);
	menus_free(gui);
	return RETURN_OK;
}

/**
 * gui_run() - The player's front, from the first window to the last.
 * @setup: what the player was started with
 *
 * Return: RETURN_OK, RETURN_FAIL when something did not come up.
 */
int gui_run(const struct gui_setup *setup)
{
	static struct gui gui;
	int result = RETURN_FAIL;

	settings_load(&gui.settings);
	gui.engine_signal = AllocSignal(-1);
	if (gui.engine_signal != -1 && timer_make(&gui))
		result = gui_serve(&gui, setup);
	if (result == RETURN_OK)
		settings_save(&gui.settings);

	engine_end(gui.engine);
	controls_free(&gui.player);

	if (gui.visual_info != NULL)
		FreeVisualInfo(gui.visual_info);
	FreeVec(gui.new_menus);
	FreeVec(gui.labels);
	if (gui.film_requester != NULL)
		FreeAslRequest(gui.film_requester);
	if (gui.subtitle_requester != NULL)
		FreeAslRequest(gui.subtitle_requester);
	if (gui.font_requester != NULL)
		FreeAslRequest(gui.font_requester);
	playlist_clear(&gui.playlist);
	timer_free(&gui);
	FreeSignal(gui.engine_signal);
	return result;
}
