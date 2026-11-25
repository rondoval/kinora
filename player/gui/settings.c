/* SPDX-License-Identifier: MPL-2.0 */
/*
 * settings.c - the player's settings, read and written (settings.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"

#define LINE_SIZE 512 /* a line of the settings file, with its NUL */
#define UNITS 4u      /* ahi.device has units 0 to 3 */

static const char *const files[] = {"ENV:Kinora.prefs", "ENVARC:Kinora.prefs"};

/**
 * line_take() - Take one name=value line into the settings.
 * @settings: the settings
 * @name:     the name
 * @value:    the value, without the line's end
 *
 * A name not known and a value out of range are passed over.
 */
static void line_take(struct settings *settings, const char *name, const char *value)
{
	ULONG number = strtoul(value, NULL, 10);
	if (strcmp(name, "unit") == 0 && number < UNITS)
		settings->audio_unit = number;
	else if (strcmp(name, "shared") == 0)
		settings->shared = number != 0;
	else if (strcmp(name, "volume") == 0 && number <= SETTINGS_VOLUME_FULL)
		settings->volume = number;
	else if (strcmp(name, "drawer") == 0 && strlen(value) < sizeof(settings->drawer))
		strcpy(settings->drawer, value);
	else if (strcmp(name, "font") == 0 && strlen(value) < sizeof(settings->font))
		strcpy(settings->font, value);
}

/**
 * settings_load() - Read the settings, or take the defaults where there are none.
 * @settings: filled in
 */
void settings_load(struct settings *settings)
{
	*settings = (struct settings){.volume = SETTINGS_VOLUME_FULL};
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
 * settings_save() - Write the settings for this run and the next.
 * @settings: the settings
 */
void settings_save(const struct settings *settings)
{
	for (ULONG i = 0; i < sizeof(files) / sizeof(files[0]); i++)
	{
		FILE *file = fopen(files[i], "w");
		if (file == NULL)
			continue;
		fprintf(file,
		        "unit=%lu\nshared=%d\nvolume=%lu\ndrawer=%s\nfont=%s\n",
		        (unsigned long)settings->audio_unit,
		        settings->shared != 0,
		        (unsigned long)settings->volume,
		        settings->drawer,
		        settings->font);
		fclose(file);
	}
}
