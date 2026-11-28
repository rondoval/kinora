/* SPDX-License-Identifier: MPL-2.0 */
/*
 * gui_rexx.c - the ARexx port KINORA.
 *
 * A command is a word and what follows it; the answer comes back as the
 * RESULT string.  The commands are the table at the end of this file's
 * first half; the user's guide lists them.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rexx/storage.h>
#include <rexx/errors.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/rexxsyslib.h>

#include "gui_private.h"
#include "../audio/audio_out.h"

#define PORT_NAME "KINORA"
#define RESULT_SIZE 2048       /* an answer, with its NUL: STATISTICS's is the longest */
#define WORD_SIZE 32           /* a command's name, with its NUL */
#define LIST_LINE_MOST 128     /* the most a line of TRACKS or CHAPTERS takes, with its NUL */
#define FILM_LINE_EXTRA 4      /* a line of PLAYLIST or RECENT beyond its path: the mark, a space, the newline, the NUL */
#define MODE_TEXT_SIZE 16      /* a screen mode as SCREENMODE answers it, with its NUL */
#define MICROSECONDS_PER_MS 1000
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

struct RxsLib *RexxSysBase;

static const char *const state_words[] = {"EMPTY", "PAUSED", "PLAYING", "ENDED"};

/* One command of the port */
struct rexx_command
{
	const char *name;                                                     /* its word, in capitals */
	void (*run)(struct gui *gui, const char *argument, char *result);     /* what does it; result holds RESULT_SIZE bytes */
};

/**
 * run_play() - PLAY: run the film.
 * @gui:      the front
 * @argument: unused
 * @result:   unused
 */
static void run_play(struct gui *gui, const char *argument, char *result)
{
	(void)argument;
	(void)result;
	engine_play(gui->engine, TRUE);
}

/**
 * run_pause() - PAUSE: stand still.
 * @gui:      the front
 * @argument: unused
 * @result:   unused
 */
static void run_pause(struct gui *gui, const char *argument, char *result)
{
	(void)argument;
	(void)result;
	engine_play(gui->engine, FALSE);
}

/**
 * run_stop() - STOP: stand still at the beginning.
 * @gui:      the front
 * @argument: unused
 * @result:   unused
 */
static void run_stop(struct gui *gui, const char *argument, char *result)
{
	(void)argument;
	(void)result;
	film_stop(gui);
}

/**
 * run_close() - CLOSE: close the film; the playlist stays.
 * @gui:      the front
 * @argument: unused
 * @result:   unused
 */
static void run_close(struct gui *gui, const char *argument, char *result)
{
	(void)argument;
	(void)result;
	film_close(gui);
}

/**
 * run_seek() - SEEK ms: move to a time of the film.
 * @gui:      the front
 * @argument: milliseconds
 * @result:   gets 0, or the engine's error
 */
static void run_seek(struct gui *gui, const char *argument, char *result)
{
	sprintf(result, "%ld", (long)engine_seek(gui->engine, (VCTime)strtol(argument, NULL, 10) * MICROSECONDS_PER_MS));
}

/**
 * run_open() - OPEN file: play a film in place of the playlist.
 * @gui:      the front
 * @argument: the file
 * @result:   unused
 */
static void run_open(struct gui *gui, const char *argument, char *result)
{
	(void)result;
	film_open(gui, (CONST_STRPTR)argument);
}

/**
 * run_add() - ADD file: put a film behind the playlist's.
 * @gui:      the front
 * @argument: the file
 * @result:   unused
 */
static void run_add(struct gui *gui, const char *argument, char *result)
{
	(void)result;
	film_add(gui, (CONST_STRPTR)argument, FALSE);
}

/**
 * run_subtitles() - SUBTITLES file: show a SubRip file's subtitles.
 * @gui:      the front
 * @argument: the file
 * @result:   gets 0, or the engine's error
 */
static void run_subtitles(struct gui *gui, const char *argument, char *result)
{
	sprintf(result, "%ld", (long)engine_subtitle_file(gui->engine, (CONST_STRPTR)argument));
}

/**
 * run_font() - FONT file: draw the subtitles with a font file; without a file, with Kinora's own.
 * @gui:      the front
 * @argument: the file, or nothing
 * @result:   gets 0, or the engine's error
 */
static void run_font(struct gui *gui, const char *argument, char *result)
{
	sprintf(result, "%ld", (long)font_set(gui, (CONST_STRPTR)argument));
	settings_window_show(gui);
}

/**
 * run_statistics() - STATISTICS [ON|OFF]: the statistics as lines of text; with a word, the panel in the picture shown or hidden too.
 * @gui:      the front
 * @argument: ON, OFF or nothing
 * @result:   gets the lines
 */
static void run_statistics(struct gui *gui, const char *argument, char *result)
{
	if (argument[0] != 0)
		gui->statistics_on = stricmp(argument, "ON") == 0;
	engine_statistics(gui->engine, gui->statistics_on, result, RESULT_SIZE);
}

/**
 * run_size() - SIZE width height: give the player's window another size, as dragging its size gadget does.
 * @gui:      the front
 * @argument: the window's width and height, its borders included
 * @result:   unused
 */
static void run_size(struct gui *gui, const char *argument, char *result)
{
	(void)result;
	char *rest = NULL;
	LONG width = strtol(argument, &rest, 10), height = strtol(rest, NULL, 10);
	struct Window *window = gui->player.window;
	if (width > 0 && height > 0)
		ChangeWindowBox(window, window->LeftEdge, window->TopEdge, (WORD)width, (WORD)height);
}

/**
 * run_next() - NEXT: the playlist's next film.
 * @gui:      the front
 * @argument: unused
 * @result:   unused
 */
static void run_next(struct gui *gui, const char *argument, char *result)
{
	(void)argument;
	(void)result;
	film_play(gui, playlist_next(&gui->playlist));
}

/**
 * run_previous() - PREVIOUS: the playlist's film before this one.
 * @gui:      the front
 * @argument: unused
 * @result:   unused
 */
static void run_previous(struct gui *gui, const char *argument, char *result)
{
	(void)argument;
	(void)result;
	film_play(gui, playlist_previous(&gui->playlist));
}

/**
 * run_fullscreen() - FULLSCREEN: move the picture to a screen of its own, or back.
 * @gui:      the front
 * @argument: unused
 * @result:   gets 1 when the picture is on the full screen now, else 0
 */
static void run_fullscreen(struct gui *gui, const char *argument, char *result)
{
	(void)argument;
	full_screen_toggle(gui);
	sprintf(result, "%d", gui->screen != NULL);
}

/**
 * run_quit() - QUIT: end the player.
 * @gui:      the front
 * @argument: unused
 * @result:   unused
 */
static void run_quit(struct gui *gui, const char *argument, char *result)
{
	(void)argument;
	(void)result;
	gui->quit = TRUE;
}

/**
 * run_status() - STATUS: what the player is doing.
 * @gui:      the front
 * @argument: unused
 * @result:   gets "state position_ms duration_ms width height shown dropped error fullscreen pointer": pointer 1 while the
 *            full screen shows the pointer (the film not playing)
 */
static void run_status(struct gui *gui, const char *argument, char *result)
{
	(void)argument;
	struct engine_status status;
	engine_status(gui->engine, &status);
	long duration = status.duration != VC_TIME_UNKNOWN ? (long)(status.duration / MICROSECONDS_PER_MS) : -1;
	sprintf(result, "%s %ld %ld %lu %lu %lu %lu %ld %d %d", state_words[status.state], (long)(status.position / MICROSECONDS_PER_MS),
	        duration, (unsigned long)status.width, (unsigned long)status.height, (unsigned long)status.frames_shown,
	        (unsigned long)status.frames_dropped, (long)status.error, gui->screen != NULL, gui->screen != NULL && !gui->film_playing);
}

/**
 * run_tracks() - TRACKS: the film's audio and subtitle tracks.
 * @gui:      the front
 * @argument: unused
 * @result:   gets a line a track: "AUDIO|SUBTITLE stream selected channels language title"
 */
static void run_tracks(struct gui *gui, const char *argument, char *result)
{
	(void)argument;
	gui->track_count = engine_tracks(gui->engine, gui->tracks, ENGINE_TRACKS);
	size_t used = 0;
	for (ULONG i = 0; i < gui->track_count && used + LIST_LINE_MOST < RESULT_SIZE; i++)
	{
		const struct engine_track *track = &gui->tracks[i];
		used += (size_t)sprintf(result + used, "%s %ld %d %lu %s %s\n", track->kind == ENGINE_TRACK_AUDIO ? "AUDIO" : "SUBTITLE",
		                        (long)track->stream, track->selected != 0, (unsigned long)track->channels,
		                        track->language[0] != 0 ? track->language : "-", track->title);
	}
}

/**
 * track_of() - The track a command's argument names.
 * @argument: a stream's number, OFF or FILE
 *
 * Return: the stream, ENGINE_TRACK_OFF or ENGINE_TRACK_FILE.
 */
static LONG track_of(const char *argument)
{
	if (stricmp(argument, "OFF") == 0)
		return ENGINE_TRACK_OFF;
	if (stricmp(argument, "FILE") == 0)
		return ENGINE_TRACK_FILE;
	return strtol(argument, NULL, 10);
}

/**
 * run_audio() - AUDIO stream|OFF: play another audio track.
 * @gui:      the front
 * @argument: the track
 * @result:   gets 0, or the engine's error
 */
static void run_audio(struct gui *gui, const char *argument, char *result)
{
	sprintf(result, "%ld", (long)engine_select(gui->engine, ENGINE_TRACK_AUDIO, track_of(argument)));
	menus_make(gui);
}

/**
 * run_subtitle() - SUBTITLE stream|OFF|FILE: show another subtitle track.
 * @gui:      the front
 * @argument: the track
 * @result:   gets 0, or the engine's error
 */
static void run_subtitle(struct gui *gui, const char *argument, char *result)
{
	sprintf(result, "%ld", (long)engine_select(gui->engine, ENGINE_TRACK_SUBTITLE, track_of(argument)));
	menus_make(gui);
}

/**
 * run_unit() - UNIT n [SHARED]: send the sound to another ahi.device unit.
 * @gui:      the front
 * @argument: the unit, and SHARED to share it with other programs (6 dB low) rather than take its hardware
 * @result:   unused
 */
static void run_unit(struct gui *gui, const char *argument, char *result)
{
	(void)result;
	unit_set(gui, (ULONG)strtol(argument, NULL, 10), strstr(argument, "SHARED") != NULL || strstr(argument, "shared") != NULL);
	settings_window_show(gui);
}

/**
 * run_volume() - VOLUME n: set the level.
 * @gui:      the front
 * @argument: 0 to 100
 * @result:   unused
 */
static void run_volume(struct gui *gui, const char *argument, char *result)
{
	(void)result;
	ULONG volume = (ULONG)strtol(argument, NULL, 10);
	gui->settings.volume = volume > VOLUME_STEPS ? VOLUME_STEPS : volume;
	engine_volume(gui->engine, gui->settings.volume * AUDIO_VOLUME_FULL / VOLUME_STEPS);
}

/**
 * run_aspect() - ASPECT width height: show the picture in another shape; 0 0 for the film's own, -1 0 to stretch.
 * @gui:      the front
 * @argument: the two numbers
 * @result:   unused
 */
static void run_aspect(struct gui *gui, const char *argument, char *result)
{
	(void)result;
	char *rest = NULL;
	LONG width = strtol(argument, &rest, 10);
	aspect_set(gui, width, strtol(rest, NULL, 10));
	menus_make(gui);
}

/**
 * run_delay() - DELAY audio_ms subtitle_ms: put the sound and the subtitles behind the picture.
 * @gui:      the front
 * @argument: the two numbers
 * @result:   unused
 */
static void run_delay(struct gui *gui, const char *argument, char *result)
{
	(void)result;
	char *rest = NULL;
	LONG audio = strtol(argument, &rest, 10);
	delays_set(gui, audio, strtol(rest, NULL, 10));
	menus_make(gui);
}

/**
 * run_chapters() - CHAPTERS: the film's chapters.
 * @gui:      the front
 * @argument: unused
 * @result:   gets a line a chapter: "start_ms title"
 */
static void run_chapters(struct gui *gui, const char *argument, char *result)
{
	(void)argument;
	gui->chapter_count = engine_chapters(gui->engine, gui->chapters, ENGINE_CHAPTERS);
	size_t used = 0;
	for (ULONG i = 0; i < gui->chapter_count && used + LIST_LINE_MOST < RESULT_SIZE; i++)
		used += (size_t)sprintf(result + used, "%ld %s\n", (long)(gui->chapters[i].start / MICROSECONDS_PER_MS), gui->chapters[i].title);
}

/**
 * playlist_change() - Change the playlist as one of PLAYLIST's words says, as the playlist window's buttons do.
 * @gui:      the front
 * @argument: SHOW, HIDE, CLEAR, or REMOVE, UP or DOWN and a film's number, the first 1; anything else changes nothing
 */
static void playlist_change(struct gui *gui, const char *argument)
{
	char *number = strchr(argument, ' ');
	ULONG film = number != NULL ? (ULONG)strtol(number, NULL, 10) - 1 : (ULONG)-1; /* past any end without a number */
	if (stricmp(argument, "SHOW") == 0)
		playlist_window_open(gui);
	else if (stricmp(argument, "HIDE") == 0)
		playlist_window_close(gui);
	else if (stricmp(argument, "CLEAR") == 0)
		playlist_clear(&gui->playlist);
	else if (strnicmp(argument, "REMOVE ", strlen("REMOVE ")) == 0)
		playlist_remove(&gui->playlist, film);
	else if (strnicmp(argument, "UP ", strlen("UP ")) == 0 || strnicmp(argument, "DOWN ", strlen("DOWN ")) == 0)
		playlist_move(&gui->playlist, film, toupper((unsigned char)argument[0]) == 'U' ? -1 : 1);
}

/**
 * run_playlist() - PLAYLIST [SHOW|HIDE|CLEAR|REMOVE n|UP n|DOWN n]: the films, the open one marked; with a word, the
 *                  playlist window shown or hidden, or the playlist changed first.
 * @gui:      the front
 * @argument: nothing, or the word; n counts the films from 1
 * @result:   gets a line a film: "* path" for the open one, "- path" for the others
 */
static void run_playlist(struct gui *gui, const char *argument, char *result)
{
	playlist_change(gui, argument);
	const struct playlist *playlist = &gui->playlist;
	size_t used = 0;
	for (ULONG i = 0; i < playlist->count && used + strlen(playlist->films[i]) + FILM_LINE_EXTRA <= RESULT_SIZE; i++)
		used += (size_t)sprintf(result + used, "%c %s\n", i == playlist->current && !playlist->open_gone ? '*' : '-', playlist->films[i]);
}

/**
 * run_recent() - RECENT [n]: the recent films, newest first; with a number, that one played in place of the playlist.
 * @gui:      the front
 * @argument: nothing, or the film's number, the newest 1
 * @result:   gets a line a film: its path, as the films were before it played
 */
static void run_recent(struct gui *gui, const char *argument, char *result)
{
	const struct settings *settings = &gui->settings;
	size_t used = 0;
	for (ULONG i = 0; i < SETTINGS_RECENT && settings->recent[i][0] != 0 && used + strlen(settings->recent[i]) + FILM_LINE_EXTRA <= RESULT_SIZE; i++)
		used += (size_t)sprintf(result + used, "%s\n", settings->recent[i]);
	ULONG film = argument[0] != 0 ? (ULONG)strtol(argument, NULL, 10) - 1 : SETTINGS_RECENT;
	if (film < SETTINGS_RECENT && settings->recent[film][0] != 0)
		film_open(gui, (CONST_STRPTR)settings->recent[film]);
}

/**
 * percent_of() - The number a command's argument names, or the one in use.
 * @argument: the number, or nothing
 * @now:      the one in use
 *
 * Return: the number.
 */
static ULONG percent_of(const char *argument, ULONG now)
{
	return argument[0] != 0 ? (ULONG)strtol(argument, NULL, 10) : now;
}

/**
 * run_subtitle_size() - SUBTITLESIZE [percent]: the subtitles' size, in percent of the normal one; with a number, set
 *                       first (within 50 and 200).
 * @gui:      the front
 * @argument: nothing, or the size
 * @result:   gets the size in use
 */
static void run_subtitle_size(struct gui *gui, const char *argument, char *result)
{
	look_set(gui, percent_of(argument, gui->settings.subtitle_size), gui->settings.subtitle_distance);
	sprintf(result, "%lu", (unsigned long)gui->settings.subtitle_size);
	settings_window_show(gui);
}

/**
 * run_subtitle_distance() - SUBTITLEDISTANCE [percent]: the subtitles' gap from the frame's edge, in percent of its
 *                           rows; with a number, set first (up to 30).
 * @gui:      the front
 * @argument: nothing, or the distance
 * @result:   gets the distance in use
 */
static void run_subtitle_distance(struct gui *gui, const char *argument, char *result)
{
	look_set(gui, gui->settings.subtitle_size, percent_of(argument, gui->settings.subtitle_distance));
	sprintf(result, "%lu", (unsigned long)gui->settings.subtitle_distance);
	settings_window_show(gui);
}

/**
 * run_screen_mode() - SCREENMODE [WORKBENCH|id]: the full screen's mode; with a word, the Workbench's or a display ID
 *                     taken first (one the full screen cannot be in is refused).
 * @gui:      the front
 * @argument: nothing, WORKBENCH, or the ID (0x for hexadecimal)
 * @result:   gets "setting mode": the setting, WORKBENCH or the ID chosen, and the mode the full screen opens in
 */
static void run_screen_mode(struct gui *gui, const char *argument, char *result)
{
	if (stricmp(argument, "WORKBENCH") == 0)
		gui->settings.screen_mode = SETTINGS_WORKBENCH_MODE;
	else if (argument[0] != 0 && display_mode_usable((ULONG)strtoul(argument, NULL, 0)))
		gui->settings.screen_mode = (ULONG)strtoul(argument, NULL, 0);
	char setting[MODE_TEXT_SIZE] = "WORKBENCH";
	if (gui->settings.screen_mode != SETTINGS_WORKBENCH_MODE)
		sprintf(setting, "0x%08lx", (unsigned long)gui->settings.screen_mode);
	sprintf(result, "%s 0x%08lx", setting, (unsigned long)full_screen_mode(gui));
	settings_window_show(gui);
}

/**
 * run_settings() - SETTINGS [SAVE|CANCEL]: open the settings window; with a word, do what its button does.
 * @gui:      the front
 * @argument: nothing, SAVE (keep the settings for later runs, close the window) or CANCEL (put back what the window
 *            opened with, close it)
 * @result:   gets 1 while the window is open, else 0
 */
static void run_settings(struct gui *gui, const char *argument, char *result)
{
	if (stricmp(argument, "SAVE") == 0)
	{
		settings_save(&gui->settings);
		settings_window_close(gui, TRUE);
	}
	else if (stricmp(argument, "CANCEL") == 0)
		settings_window_close(gui, FALSE);
	else
		settings_window_open(gui);
	sprintf(result, "%d", gui->settings_window.window != NULL);
}

/* The commands, by their word */
static const struct rexx_command commands[] = {
    {"PLAY",       run_play},
    {"PAUSE",      run_pause},
    {"STOP",       run_stop},
    {"SEEK",       run_seek},
    {"OPEN",       run_open},
    {"CLOSE",      run_close},
    {"ADD",        run_add},
    {"SUBTITLES",  run_subtitles},
    {"NEXT",       run_next},
    {"PREVIOUS",   run_previous},
    {"FULLSCREEN", run_fullscreen},
    {"QUIT",       run_quit},
    {"STATUS",     run_status},
    {"TRACKS",     run_tracks},
    {"AUDIO",      run_audio},
    {"SUBTITLE",   run_subtitle},
    {"UNIT",       run_unit},
    {"VOLUME",     run_volume},
    {"ASPECT",     run_aspect},
    {"DELAY",      run_delay},
    {"CHAPTERS",   run_chapters},
    {"PLAYLIST",   run_playlist},
    {"FONT",       run_font},
    {"STATISTICS", run_statistics},
    {"SIZE",       run_size},
    {"RECENT",     run_recent},
    {"SUBTITLESIZE", run_subtitle_size},
    {"SUBTITLEDISTANCE", run_subtitle_distance},
    {"SCREENMODE", run_screen_mode},
    {"SETTINGS",   run_settings},
};

/**
 * command_run() - Run one command line.
 * @gui:    the front
 * @line:   the line: a word, then its argument
 * @result: gets the answer, RESULT_SIZE bytes; empty for a command that has none
 *
 * Return: TRUE, FALSE for a word that is no command.
 */
static BOOL command_run(struct gui *gui, const char *line, char *result)
{
	char word[WORD_SIZE];
	size_t length = 0;
	while (line[length] != 0 && line[length] != ' ' && length + 1 < sizeof(word))
	{
		word[length] = (char)toupper((unsigned char)line[length]);
		length++;
	}
	word[length] = 0;
	const char *argument = line + length;
	while (*argument == ' ')
		argument++;
	for (ULONG i = 0; i < ARRAY_COUNT(commands); i++)
		if (strcmp(commands[i].name, word) == 0)
		{
			commands[i].run(gui, argument, result);
			return TRUE;
		}
	return FALSE;
}

/**
 * rexx_open() - Make the ARexx port.
 * @gui: the front
 *
 * Without rexxsyslib.library, or with the name taken by another Kinora, there is no port.
 */
void rexx_open(struct gui *gui)
{
	RexxSysBase = (struct RxsLib *)OpenLibrary((CONST_STRPTR) "rexxsyslib.library", 0);
	if (RexxSysBase == NULL)
		return;
	Forbid();
	if (FindPort((CONST_STRPTR)PORT_NAME) == NULL)
	{
		gui->rexx_port = CreateMsgPort();
		if (gui->rexx_port != NULL)
		{
			gui->rexx_port->mp_Node.ln_Name = (char *)PORT_NAME;
			AddPort(gui->rexx_port);
		}
	}
	Permit();
}

/**
 * rexx_close() - Take the ARexx port away, the commands still waiting at it refused.
 * @gui: the front
 */
void rexx_close(struct gui *gui)
{
	if (gui->rexx_port != NULL)
	{
		RemPort(gui->rexx_port);
		struct RexxMsg *message;
		while ((message = (struct RexxMsg *)GetMsg(gui->rexx_port)) != NULL)
		{
			message->rm_Result1 = RC_FATAL;
			ReplyMsg(&message->rm_Node);
		}
		DeleteMsgPort(gui->rexx_port);
	}
	gui->rexx_port = NULL;
	if (RexxSysBase != NULL)
		CloseLibrary((struct Library *)RexxSysBase);
	RexxSysBase = NULL;
}

/**
 * rexx_take() - Run the commands that came to the ARexx port and answer them.
 * @gui: the front
 */
void rexx_take(struct gui *gui)
{
	struct RexxMsg *message;
	while (gui->rexx_port != NULL && (message = (struct RexxMsg *)GetMsg(gui->rexx_port)) != NULL)
	{
		static char result[RESULT_SIZE];
		result[0] = 0;
		BOOL known = command_run(gui, (const char *)message->rm_Args[0], result);
		message->rm_Result1 = known ? RC_OK : RC_ERROR;
		message->rm_Result2 = 0;
		if (known && (message->rm_Action & RXFF_RESULT) != 0)
			message->rm_Result2 = (LONG)CreateArgstring((CONST_STRPTR)result, strlen(result));
		ReplyMsg(&message->rm_Node);
	}
}
