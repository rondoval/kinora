/* SPDX-License-Identifier: MPL-2.0 */
/*
 * settings.h - what the player remembers from one run to the next, kept as
 * lines of name=value in ENVARC:Kinora.prefs (and ENV:).
 */
#ifndef KINORA_SETTINGS_H
#define KINORA_SETTINGS_H

#include <exec/types.h>
#include <graphics/modeid.h>
#include <intuition/intuition.h>

#define SETTINGS_DRAWER_SIZE 256 /* the last drawer a film was opened from, with its NUL */
#define SETTINGS_FONT_SIZE 256   /* the subtitles' font file, with its NUL */
#define SETTINGS_PATH_SIZE 512   /* a recent film's path, with its NUL */
#define SETTINGS_RECENT 5u       /* the recent films kept */
#define SETTINGS_VOLUME_FULL 100u
#define SETTINGS_UNITS 4u        /* ahi.device has units 0 to 3 */
#define SETTINGS_WORKBENCH_MODE ((ULONG)INVALID_ID) /* settings.screen_mode: the full screen takes the Workbench's mode */

/* The settings */
struct settings
{
	ULONG audio_unit;                   /* the ahi.device unit the sound goes to */
	BOOL shared;                        /* share that unit with other programs, 6 dB low; else take its hardware */
	ULONG volume;                       /* 0 to SETTINGS_VOLUME_FULL */
	char drawer[SETTINGS_DRAWER_SIZE];  /* where the file requester starts */
	char font[SETTINGS_FONT_SIZE];      /* the font file subtitles are drawn with; empty for Kinora's own */
	ULONG subtitle_size;                /* the subtitles' size in percent of the normal one, SUBTITLE_SIZE_LEAST to _MOST */
	ULONG subtitle_distance;            /* their gap from the frame's edge in percent of its rows, up to SUBTITLE_DISTANCE_MOST */
	ULONG screen_mode;                  /* the full screen's display ID; SETTINGS_WORKBENCH_MODE for the Workbench's */
	char recent[SETTINGS_RECENT][SETTINGS_PATH_SIZE]; /* the films that last started playing, newest first; empty past the last */
	struct IBox playlist_box;           /* where the playlist window last stood and its size; Width 0 for never opened */
};

void settings_load(struct settings *settings);
void settings_save(const struct settings *settings);
void settings_recent_add(struct settings *settings, const char *path);

#endif /* KINORA_SETTINGS_H */
