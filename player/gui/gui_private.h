/* SPDX-License-Identifier: MPL-2.0 */
/*
 * gui_private.h - the front's own structures.
 *
 *   gui.c            the window, its gadgets, the event loop, what a gadget or a key does
 *   gui_menu.c       the menus, made again whenever the film, its tracks or the playlist change
 *   gui_screen.c     full screen: the screen, and the controls shown over the picture's foot while the pointer moves
 *   gui_rexx.c       the ARexx port
 *   gui_wb.c         icons dropped on the window
 *   gui_jump.c       the "jump to time" requester
 *   gui_playlist.c   the films to play, one after the other
 *   gui_list.c       the playlist window
 *   gui_settings.c   the settings window
 *   settings.c       what is remembered between runs
 *
 * The front's task only sends the engine commands (engine.h) and asks it
 * four times a second where the film is.
 */
#ifndef KINORA_GUI_PRIVATE_H
#define KINORA_GUI_PRIVATE_H

#include <exec/ports.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <intuition/classusr.h>
#include <libraries/asl.h>
#include <libraries/gadtools.h>
#include <utility/hooks.h>
#include <exec/lists.h>

#include "gui.h"
#include "settings.h"
#include "../engine/engine.h"
#include "../subtitles/subtitles.h"

#define STATUS_SIZE 96        /* the status line, with its NUL */
#define PATH_SIZE 512         /* a file's path, with its NUL */
#define OWN_FONT "Fonts/DejaVuSans.ttf" /* the font subtitles are drawn with unless another is chosen, from the program's drawer */
#define FONT_PATTERN "#?.(ttf|otf)" /* the font files the requesters show at first: what FreeType reads */
#define MENU_LABEL_SIZE 80    /* a menu item's text, with its NUL */
#define PLAYLIST_FILMS 256u   /* the films a playlist holds */
#define SEEK_STEPS 1000       /* the seek slider's range */
#define VOLUME_STEPS 100      /* the volume slider's range */

/* The gadgets */
enum gadget_id
{
	GID_PICTURE = 1,
	GID_SEEK,
	GID_PREVIOUS,
	GID_PLAY,
	GID_STOP,
	GID_NEXT,
	GID_VOLUME,
	GID_STATUS,
	GID_COUNT
};

/* The settings window's gadgets */
enum setting_gadget
{
	SETTING_UNIT = 1,
	SETTING_SHARED,
	SETTING_FONT,
	SETTING_FONT_OWN,
	SETTING_SIZE,
	SETTING_DISTANCE,
	SETTING_AS_WORKBENCH,
	SETTING_MODE,
	SETTING_SAVE,
	SETTING_CANCEL,
	SETTING_COUNT
};

/* The playlist window's gadgets */
enum list_gadget
{
	LIST_FILMS = 1,
	LIST_ADD,
	LIST_REMOVE,
	LIST_UP,
	LIST_DOWN,
	LIST_CLEAR,
	LIST_COUNT
};

/* Where the Workbench says icons were dropped (AppMessage.am_ID) */
enum drop_place
{
	DROP_PLAYER,   /* the player's window: they replace the playlist and the first plays */
	DROP_PLAYLIST, /* the playlist window: they go behind its films */
};

/* A window with the controls: the player's window, or the strip shown over a full screen's foot */
struct controls
{
	Object *window_object;             /* the window.class object, NULL while there is none */
	struct Window *window;             /* its window while open */
	struct Gadget *gadgets[GID_COUNT]; /* its gadgets by their numbers; the strip has no picture box */
	char status[STATUS_SIZE];          /* its status line's text */
	CONST_STRPTR play_label;           /* the play button's label: "Play" while the film stands still, "Pause" while it plays */
};

/* The films to play */
struct playlist
{
	char *films[PLAYLIST_FILMS]; /* their paths */
	ULONG count;                 /* how many */
	ULONG current;               /* the one open; with open_gone, where it stood */
	BOOL open_gone;              /* the open film was taken out of the list: the film at current is the next */
	ULONG changes;               /* counts every change, for the playlist window to follow */
};

/* The settings window, while open */
struct settings_window
{
	Object *window_object;                /* the window.class object, NULL while closed */
	struct Window *window;                /* its window while open */
	struct Gadget *gadgets[SETTING_COUNT]; /* its gadgets by their numbers */
	struct settings before;               /* the settings as it found them: Cancel puts them back */
	struct Hook mode_filter;              /* keeps the screen mode requester to the modes the full screen can be in */
	char font_path[PATH_SIZE];            /* the font file its gadget shows, kept while it shows it */
};

/* The playlist window, while open */
struct playlist_window
{
	Object *window_object;              /* the window.class object, NULL while closed */
	struct Window *window;              /* its window while open */
	struct Gadget *gadgets[LIST_COUNT]; /* its gadgets by their numbers */
	struct List lines;                  /* the list browser's nodes, a film each */
	ULONG shown_changes;                /* the playlist's changes the lines were made at */
	LONG selected;                      /* the film selected, -1 for none */
	struct AppWindow *drop_window;      /* the window as one icons can be dropped on */
};

/* The front */
struct gui
{
	/* The engine */
	struct engine *engine;       /* the engine */
	BYTE engine_signal;          /* what the engine signals the front's task with */

	/* The player's window and its menus */
	struct controls player;      /* the player's window */
	struct Menu *menus;          /* the window's menus */
	APTR visual_info;            /* for making them */
	struct NewMenu *new_menus;   /* what they are made from */
	char (*labels)[MENU_LABEL_SIZE]; /* their items' texts, copied */
	struct FileRequester *film_requester;     /* the Open requester, kept for its drawer */
	struct FileRequester *subtitle_requester; /* the subtitle file's */
	ULONG click_seconds;          /* when the picture was last clicked, for telling a double click; 0 for not lately */
	ULONG click_micros;           /* and the microseconds of that */
	BOOL quit;                    /* the window was asked to close */

	/* The full screen */
	struct Screen *screen;        /* the full screen while the picture is on it, else NULL */
	struct Window *screen_window; /* the window on it that takes the keys and the pointer's moves */
	struct controls strip;        /* the controls over a full screen's foot, while shown */
	ULONG screen_width;           /* the screen's visible pixels */
	ULONG screen_height;          /* and rows */
	WORD pointer_x;               /* where the pointer last was on it */
	WORD pointer_y;               /* and how far down */
	ULONG strip_idle_ticks;       /* ticks since the pointer last moved or a control was used over the full screen */

	/* The tick, and what it keeps going */
	struct MsgPort *timer_port;  /* the tick's port */
	struct timerequest *timer;   /* the tick */
	BOOL timer_open;             /* timer.device is open on it */
	BOOL timer_sent;             /* a tick is out */

	/* Where else commands come from */
	struct MsgPort *rexx_port;    /* the ARexx port, NULL when another Kinora has the name */
	struct MsgPort *drop_port;    /* where the Workbench says what was dropped on the window */
	struct AppWindow *drop_window; /* the window as one icons can be dropped on */

	/* The other windows */
	struct settings_window settings_window; /* the settings window */
	struct playlist_window playlist_window; /* the playlist window */

	/* The films and the one that is open */
	struct playlist playlist;     /* the films */
	struct engine_track tracks[ENGINE_TRACKS]; /* the open film's tracks, as the menus list them */
	ULONG track_count;            /* how many */
	struct engine_chapter chapters[ENGINE_CHAPTERS]; /* the open film's chapters, as the menu lists them */
	ULONG chapter_count;          /* how many */

	/* How it is played */
	struct settings settings;     /* what is remembered between runs */
	LONG aspect_width;            /* the shape the picture is shown in: 0 for the film's own, DISPLAY_ASPECT_FILL to stretch */
	LONG aspect_height;           /* to this height */
	LONG audio_delay;             /* milliseconds the sound is put behind the picture */
	LONG subtitle_delay;          /* milliseconds the subtitles are */
	BOOL statistics_on;           /* the statistics panel is drawn into the picture */
	BOOL film_playing;            /* the film played when the front last looked (film_follow()); the full screen's pointer hides while it does */
};

/* gui.c */
struct Gadget *button_make(ULONG id, CONST_STRPTR text);
ULONG window_signals(Object *window_object);
void gadget_set(struct Gadget *gadget, struct Window *window, Tag tag, ULONG value);
BOOL controls_make(struct controls *controls, struct Screen *screen, ULONG volume);
void controls_free(struct controls *controls);
void controls_input_take(struct gui *gui, struct controls *controls);
void film_follow(struct gui *gui);
void picture_box(struct gui *gui, struct display_place *place);
void key_used(struct gui *gui, ULONG code);
void film_play(struct gui *gui, ULONG index);
void film_open(struct gui *gui, CONST_STRPTR name);
void film_close(struct gui *gui);
void films_ask(struct gui *gui, BOOL add);
void subtitle_ask(struct gui *gui);
LONG font_set(struct gui *gui, CONST_STRPTR name);
BOOL font_file(CONST_STRPTR name, char *path);
void look_set(struct gui *gui, ULONG size, ULONG distance);
void unit_set(struct gui *gui, ULONG unit, BOOL shared);
void play_toggle(struct gui *gui);
void film_add(struct gui *gui, CONST_STRPTR name, BOOL play);
void aspect_set(struct gui *gui, LONG width, LONG height);
void delays_set(struct gui *gui, LONG audio, LONG subtitles);
void film_stop(struct gui *gui);
void statistics_toggle(struct gui *gui);
BOOL click_doubled(struct gui *gui);

/* gui_menu.c */
void menus_make(struct gui *gui);
void menus_free(struct gui *gui);
void menu_used(struct gui *gui, UWORD code);

/* gui_screen.c */
void full_screen_toggle(struct gui *gui);
void full_screen_close(struct gui *gui);
void full_screen_place(const struct gui *gui, struct display_place *place);
void full_screen_input_take(struct gui *gui);
void full_screen_tick(struct gui *gui);
void full_screen_pointer(const struct gui *gui);
ULONG full_screen_mode(const struct gui *gui);
ULONG full_screen_signals(const struct gui *gui);

/* gui_rexx.c */
void rexx_open(struct gui *gui);
void rexx_close(struct gui *gui);
void rexx_take(struct gui *gui);

/* gui_wb.c */
void drop_open(struct gui *gui);
void drop_close(struct gui *gui);
void drop_take(struct gui *gui);
struct AppWindow *drop_add(struct gui *gui, struct Window *window, enum drop_place place);
void drop_remove(struct AppWindow *drop_window);

/* gui_jump.c */
void jump_ask(struct gui *gui);

/* gui_playlist.c */
BOOL playlist_add(struct playlist *playlist, CONST_STRPTR name);
void playlist_clear(struct playlist *playlist);
void playlist_choose(struct playlist *playlist, ULONG index);
void playlist_remove(struct playlist *playlist, ULONG index);
ULONG playlist_move(struct playlist *playlist, ULONG index, LONG step);
ULONG playlist_next(const struct playlist *playlist);
ULONG playlist_previous(const struct playlist *playlist);
CONST_STRPTR playlist_name(const struct playlist *playlist, ULONG index);

/* gui_list.c */
void playlist_window_open(struct gui *gui);
void playlist_window_close(struct gui *gui);
void playlist_window_follow(struct gui *gui);
void playlist_window_input_take(struct gui *gui);

/* gui_settings.c */
void settings_window_open(struct gui *gui);
void settings_window_close(struct gui *gui, BOOL keep);
void settings_window_show(struct gui *gui);
void settings_window_input_take(struct gui *gui);

#endif /* KINORA_GUI_PRIVATE_H */
