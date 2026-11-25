/* SPDX-License-Identifier: MPL-2.0 */
/*
 * settings.h - what the player remembers from one run to the next, kept as
 * lines of name=value in ENVARC:Kinora.prefs (and ENV:).
 */
#ifndef KINORA_SETTINGS_H
#define KINORA_SETTINGS_H

#include <exec/types.h>

#define SETTINGS_DRAWER_SIZE 256 /* the last drawer a film was opened from, with its NUL */
#define SETTINGS_FONT_SIZE 256   /* the subtitles' font file, with its NUL */
#define SETTINGS_VOLUME_FULL 100u

/* The settings */
struct settings
{
	ULONG audio_unit;                   /* the ahi.device unit the sound goes to */
	BOOL shared;                        /* share that unit with other programs, 6 dB low; else take its hardware */
	ULONG volume;                       /* 0 to SETTINGS_VOLUME_FULL */
	char drawer[SETTINGS_DRAWER_SIZE];  /* where the file requester starts */
	char font[SETTINGS_FONT_SIZE];      /* the font file subtitles are drawn with; empty for Kinora's own */
};

void settings_load(struct settings *settings);
void settings_save(const struct settings *settings);

#endif /* KINORA_SETTINGS_H */
