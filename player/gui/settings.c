/* SPDX-License-Identifier: MPL-2.0 */
/*
 * settings.c - the player's settings, read and written (settings.h).
 *
 * Numbers and texts go by tables; a name not known and a value out of range
 * are passed over, so a file from another version reads as far as it goes.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"
#include "../subtitles/subtitles.h"

#define LINE_SIZE (SETTINGS_PATH_SIZE + 32) /* a line of the settings file, with its NUL: a name and a path */
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

static const char *const files[] = {"ENV:Kinora.prefs", "ENVARC:Kinora.prefs"};

/* A setting that is a number */
struct number_setting
{
	const char *name; /* its name in the file */
	size_t offset;    /* its ULONG in struct settings */
	ULONG least;      /* the lowest value taken */
	ULONG most;       /* the highest */
};

static const struct number_setting numbers[] = {
    {"unit", offsetof(struct settings, audio_unit), 0, SETTINGS_UNITS - 1},
    {"volume", offsetof(struct settings, volume), 0, SETTINGS_VOLUME_FULL},
    {"subtitlesize", offsetof(struct settings, subtitle_size), SUBTITLE_SIZE_LEAST, SUBTITLE_SIZE_MOST},
    {"subtitledistance", offsetof(struct settings, subtitle_distance), 0, SUBTITLE_DISTANCE_MOST},
};

/* A setting that is a text */
struct text_setting
{
	const char *name; /* its name in the file */
	size_t offset;    /* its characters in struct settings */
	size_t size;      /* and their room, with the NUL */
};

static const struct text_setting texts[] = {
    {"drawer", offsetof(struct settings, drawer), SETTINGS_DRAWER_SIZE},
    {"font", offsetof(struct settings, font), SETTINGS_FONT_SIZE},
};

/**
 * number_take() - Take a line that is one of the numbers.
 * @settings: the settings
 * @name:     the line's name
 * @value:    its value
 *
 * Return: TRUE if the name is one of numbers[], taken or, out of range, passed over.
 */
static BOOL number_take(struct settings *settings, const char *name, const char *value)
{
	for (ULONG i = 0; i < ARRAY_COUNT(numbers); i++)
		if (strcmp(name, numbers[i].name) == 0)
		{
			ULONG number = strtoul(value, NULL, 10);
			if (number >= numbers[i].least && number <= numbers[i].most)
				*(ULONG *)((char *)settings + numbers[i].offset) = number;
			return TRUE;
		}
	return FALSE;
}

/**
 * text_take() - Take a line that is one of the texts.
 * @settings: the settings
 * @name:     the line's name
 * @value:    its value
 *
 * Return: TRUE if the name is one of texts[], taken or, too long, passed over.
 */
static BOOL text_take(struct settings *settings, const char *name, const char *value)
{
	for (ULONG i = 0; i < ARRAY_COUNT(texts); i++)
		if (strcmp(name, texts[i].name) == 0)
		{
			if (strlen(value) < texts[i].size)
				strcpy((char *)settings + texts[i].offset, value);
			return TRUE;
		}
	return FALSE;
}

/**
 * recent_take() - Take a recent film: behind the ones taken so far.
 * @settings: the settings
 * @value:    the film's path; one too long, or past SETTINGS_RECENT, is passed over
 */
static void recent_take(struct settings *settings, const char *value)
{
	if (value[0] == 0 || strlen(value) >= SETTINGS_PATH_SIZE)
		return;
	for (ULONG i = 0; i < SETTINGS_RECENT; i++)
		if (settings->recent[i][0] == 0)
		{
			strcpy(settings->recent[i], value);
			return;
		}
}

/**
 * box_take() - Take a window's place and size.
 * @box:   gets them; left as it is when the value does not hold four numbers, the size above 0
 * @value: "left top width height"
 */
static void box_take(struct IBox *box, const char *value)
{
	char *rest = NULL;
	LONG left = strtol(value, &rest, 10), top = strtol(rest, &rest, 10);
	LONG width = strtol(rest, &rest, 10), height = strtol(rest, NULL, 10);
	if (width > 0 && height > 0)
		*box = (struct IBox){.Left = (WORD)left, .Top = (WORD)top, .Width = (WORD)width, .Height = (WORD)height};
}

/**
 * line_take() - Take one name=value line into the settings.
 * @settings: the settings
 * @name:     the name
 * @value:    the value, without the line's end
 */
static void line_take(struct settings *settings, const char *name, const char *value)
{
	if (number_take(settings, name, value) || text_take(settings, name, value))
		return;
	if (strcmp(name, "shared") == 0)
		settings->shared = strtoul(value, NULL, 10) != 0;
	else if (strcmp(name, "screenmode") == 0)
		settings->screen_mode = value[0] != 0 ? strtoul(value, NULL, 0) : SETTINGS_WORKBENCH_MODE;
	else if (strcmp(name, "recent") == 0)
		recent_take(settings, value);
	else if (strcmp(name, "playlistwindow") == 0)
		box_take(&settings->playlist_box, value);
}

/**
 * settings_load() - Read the settings, or take the defaults where there are none.
 * @settings: filled in
 */
void settings_load(struct settings *settings)
{
	*settings = (struct settings){
	    .volume = SETTINGS_VOLUME_FULL,
	    .subtitle_size = SUBTITLE_SIZE_NORMAL,
	    .subtitle_distance = SUBTITLE_DISTANCE_NORMAL,
	    .screen_mode = SETTINGS_WORKBENCH_MODE,
	};
	FILE *file = fopen(files[0], "r");
	if (file == NULL)
		return;
	char line[LINE_SIZE];
	while (fgets(line, sizeof(line), file) != NULL)
	{
		line[strcspn(line, "\r\n")] = 0;
		char *equals = strchr(line, '=');
		if (equals == NULL)
			continue;
		*equals = 0;
		line_take(settings, line, equals + 1);
	}
	fclose(file);
}

/**
 * settings_write() - Write the settings into one file.
 * @settings: the settings
 * @file:     the file, open for writing
 */
static void settings_write(const struct settings *settings, FILE *file)
{
	for (ULONG i = 0; i < ARRAY_COUNT(numbers); i++)
		fprintf(file, "%s=%lu\n", numbers[i].name, (unsigned long)*(const ULONG *)((const char *)settings + numbers[i].offset));
	for (ULONG i = 0; i < ARRAY_COUNT(texts); i++)
		fprintf(file, "%s=%s\n", texts[i].name, (const char *)settings + texts[i].offset);
	fprintf(file, "shared=%d\n", settings->shared != 0);
	if (settings->screen_mode == SETTINGS_WORKBENCH_MODE)
		fprintf(file, "screenmode=\n");
	else
		fprintf(file, "screenmode=0x%08lx\n", (unsigned long)settings->screen_mode);
	for (ULONG i = 0; i < SETTINGS_RECENT && settings->recent[i][0] != 0; i++)
		fprintf(file, "recent=%s\n", settings->recent[i]);
	const struct IBox *box = &settings->playlist_box;
	if (box->Width > 0)
		fprintf(file, "playlistwindow=%d %d %d %d\n", box->Left, box->Top, box->Width, box->Height);
}

/**
 * settings_save() - Write the settings for this run and the next.
 * @settings: the settings
 */
void settings_save(const struct settings *settings)
{
	for (ULONG i = 0; i < ARRAY_COUNT(files); i++)
	{
		FILE *file = fopen(files[i], "w");
		if (file == NULL)
			continue;
		settings_write(settings, file);
		fclose(file);
	}
}

/**
 * settings_recent_add() - Put a film first among the recent ones.
 * @settings: the settings
 * @path:     the film's path; one too long to keep is passed over
 *
 * A film there already moves to the front; else the oldest goes.
 */
void settings_recent_add(struct settings *settings, const char *path)
{
	if (strlen(path) >= SETTINGS_PATH_SIZE)
		return;
	ULONG dropped = SETTINGS_RECENT - 1; /* the entry that makes room: the film's own, else the oldest */
	for (ULONG i = 0; i < SETTINGS_RECENT; i++)
		if (strcmp(settings->recent[i], path) == 0)
		{
			dropped = i;
			break;
		}
	memmove(settings->recent[1], settings->recent[0], dropped * SETTINGS_PATH_SIZE);
	strcpy(settings->recent[0], path);
}
