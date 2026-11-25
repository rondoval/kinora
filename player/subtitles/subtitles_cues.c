/* SPDX-License-Identifier: MPL-2.0 */
/*
 * subtitles_cues.c - the cues: added from a subtitle stream's packets or
 * read from a SubRip file, and those a time of the film lies in found.
 *
 * The cues are kept by their start.  Each knows its reach, the latest end
 * among it and the cues before it, so the search for a time looks back only
 * as far as a cue might still be shown.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "subtitles_private.h"

#define CUES_FIRST 256u               /* the cues the array holds at first; it doubles when full */
#define CUE_DEFAULT_TIME 3000000      /* microseconds a cue is shown when its packet names no duration */
#define LINE_SIZE 1024                /* a line of a SubRip file, with its NUL */
#define MICROSECONDS_PER_MS 1000
#define MS_PER_SECOND 1000
#define SECONDS_PER_MINUTE 60
#define MINUTES_PER_HOUR 60
#define TIME_ARROW "-->"              /* a SubRip time line: between the start and the end */
#define TIME_ARROW_LENGTH (sizeof(TIME_ARROW) - 1)
#define ENGINE_ERR_FILE (-2)          /* as engine.h's: the file did not open or holds no cue */

/**
 * cue_after() - Where the cues that start after a time begin.
 * @subtitles: the subtitles
 * @time:      the time
 *
 * Return: the index of the first cue that starts after the time, cue_count when none does.
 */
static ULONG cue_after(const struct subtitles *subtitles, VCTime time)
{
	ULONG low = 0;
	ULONG high = subtitles->cue_count;
	while (low < high)
	{
		ULONG middle = low + (high - low) / 2;
		if (subtitles->cues[middle].start <= time)
			low = middle + 1;
		else
			high = middle;
	}
	return low;
}

/**
 * cue_known() - Whether the cue is there already.
 * @subtitles: the subtitles
 * @place:     cue_after() of the start: the cues of that start lie just before it
 * @start:     when it appears
 * @end:       when it goes
 * @text:      its plain text
 *
 * Return: TRUE if a cue of the same times and text is.
 */
static BOOL cue_known(const struct subtitles *subtitles, ULONG place, VCTime start, VCTime end, const char *text)
{
	for (ULONG i = place; i > 0 && subtitles->cues[i - 1].start == start; i--)
		if (subtitles->cues[i - 1].end == end && strcmp(subtitles->cues[i - 1].text, text) == 0)
			return TRUE;
	return FALSE;
}

/**
 * cues_grow() - Double the room for cues.
 * @subtitles: the subtitles
 *
 * Return: TRUE, FALSE without memory.
 */
static BOOL cues_grow(struct subtitles *subtitles)
{
	ULONG room = subtitles->cue_room != 0 ? subtitles->cue_room * 2 : CUES_FIRST;
	struct cue *cues = AllocVec(room * sizeof(*cues), MEMF_ANY);
	if (cues == NULL)
		return FALSE;
	if (subtitles->cue_count != 0)
		memcpy(cues, subtitles->cues, subtitles->cue_count * sizeof(*cues));
	FreeVec(subtitles->cues);
	subtitles->cues = cues;
	subtitles->cue_room = room;
	return TRUE;
}

/**
 * reaches_update() - Set the reach of the cues from one on.
 * @subtitles: the subtitles
 * @first:     the first cue whose reach may have changed
 */
static void reaches_update(struct subtitles *subtitles, ULONG first)
{
	for (ULONG i = first; i < subtitles->cue_count; i++)
	{
		struct cue *cue = &subtitles->cues[i];
		VCTime before = i > 0 ? subtitles->cues[i - 1].reach : cue->end;
		cue->reach = before > cue->end ? before : cue->end;
	}
}

/**
 * cues_add() - Add a cue in its place by its start.
 * @subtitles: the subtitles
 * @start:     when it appears
 * @end:       when it goes
 * @text:      its plain text; an empty one adds nothing
 *
 * A cue of the same times and text is there already when the film was
 * moved back over it and its packet came again: it is not added twice.
 * Cues nearly always come in order and go at the end; one that comes early
 * moves those after it.
 *
 * Return: TRUE, FALSE without memory.
 */
static BOOL cues_add(struct subtitles *subtitles, VCTime start, VCTime end, const char *text)
{
	if (text[0] == 0)
		return TRUE;
	ULONG place = cue_after(subtitles, start);
	if (cue_known(subtitles, place, start, end, text))
		return TRUE;
	if (subtitles->cue_count == subtitles->cue_room && !cues_grow(subtitles))
		return FALSE;
	char *copy = AllocVec(strlen(text) + 1, MEMF_ANY);
	if (copy == NULL)
		return FALSE;
	strcpy(copy, text);
	if (place < subtitles->cue_count)
		memmove(&subtitles->cues[place + 1], &subtitles->cues[place], (subtitles->cue_count - place) * sizeof(struct cue));
	subtitles->cues[place] = (struct cue){
	    .start = start, .end = end, .serial = ++subtitles->last_serial, .top = strchr(copy, MARK_TOP) != NULL, .text = copy};
	subtitles->cue_count++;
	reaches_update(subtitles, place);
	return TRUE;
}

/**
 * cues_at() - The cues shown at a time.
 * @subtitles: the subtitles
 * @time:      the film's time
 * @shown:     gets the cues the time lies in, by their start
 * @room:      how many it holds; of more cues, those that started last are taken
 *
 * It looks back from the last cue that starts by the time, and stops where
 * no cue so far reaches past the time.
 *
 * Return: how many there are.
 */
ULONG cues_at(const struct subtitles *subtitles, VCTime time, const struct cue **shown, ULONG room)
{
	ULONG count = 0;
	for (ULONG i = cue_after(subtitles, time); i > 0 && subtitles->cues[i - 1].reach > time && count < room; i--)
		if (time < subtitles->cues[i - 1].end)
			shown[count++] = &subtitles->cues[i - 1];
	for (ULONG i = 0; i < count / 2; i++) /* found the last first: turn them round */
	{
		const struct cue *cue = shown[i];
		shown[i] = shown[count - 1 - i];
		shown[count - 1 - i] = cue;
	}
	return count;
}

/**
 * subtitles_flush() - Forget every cue.
 * @subtitles: the subtitles
 */
void subtitles_flush(struct subtitles *subtitles)
{
	for (ULONG i = 0; i < subtitles->cue_count; i++)
		FreeVec(subtitles->cues[i].text);
	subtitles->cue_count = 0;
}

/**
 * subtitles_add_packet() - Add the cue a subtitle stream's packet holds.
 * @subtitles: the subtitles
 * @codec:     DMXS_*: only the text kinds are taken
 * @packet:    the packet
 */
void subtitles_add_packet(struct subtitles *subtitles, ULONG codec, const struct DMXPacket *packet)
{
	if ((codec != DMXS_Text && codec != DMXS_ASS && codec != DMXS_MovText) || packet->Pts == VC_TIME_UNKNOWN)
		return;
	char plain[SUBTITLE_TEXT_SIZE];
	text_plain(plain, sizeof(plain), packet->Data, packet->Length, codec);
	VCTime shown = packet->Duration != VC_TIME_UNKNOWN && packet->Duration > 0 ? packet->Duration : CUE_DEFAULT_TIME;
	cues_add(subtitles, packet->Pts, packet->Pts + shown, plain);
}

/**
 * time_read() - Read one of a SubRip line's two times.
 * @text: "hh:mm:ss,mmm"
 * @time: gets the time
 *
 * Return: TRUE when the text is a time.
 */
static BOOL time_read(const char *text, VCTime *time)
{
	unsigned hours, minutes, seconds, milliseconds;
	char separator;
	if (sscanf(text, "%u:%u:%u%c%u", &hours, &minutes, &seconds, &separator, &milliseconds) != 5)
		return FALSE;
	VCTime whole = ((VCTime)hours * MINUTES_PER_HOUR + minutes) * SECONDS_PER_MINUTE + seconds;
	*time = (whole * MS_PER_SECOND + milliseconds) * MICROSECONDS_PER_MS;
	return TRUE;
}

/**
 * times_read() - Read a SubRip cue's time line.
 * @line:  "00:00:01,000 --> 00:00:04,000"
 * @start: gets the first time
 * @end:   gets the second
 *
 * Return: TRUE when the line is one.
 */
static BOOL times_read(const char *line, VCTime *start, VCTime *end)
{
	const char *arrow = strstr(line, TIME_ARROW);
	return arrow != NULL && time_read(line, start) && time_read(arrow + TIME_ARROW_LENGTH, end);
}

/**
 * cue_read() - Read the text lines of one SubRip cue and add it.
 * @subtitles: the subtitles
 * @file:      the file, behind the cue's time line
 * @start:     when the cue appears
 * @end:       when it goes
 */
static void cue_read(struct subtitles *subtitles, FILE *file, VCTime start, VCTime end)
{
	char line[LINE_SIZE], text[LINE_SIZE * 2] = "";
	while (fgets(line, sizeof(line), file) != NULL && line[0] != '\n' && line[0] != '\r')
		if (strlen(text) + strlen(line) < sizeof(text))
			strcat(text, line);
	char plain[SUBTITLE_TEXT_SIZE];
	text_plain(plain, sizeof(plain), text, strlen(text), DMXS_Text);
	cues_add(subtitles, start, end, plain);
}

/**
 * subtitles_load() - Read a SubRip file as the film's subtitles, in place of those there.
 * @subtitles: the subtitles
 * @name:      the file
 *
 * Return: 0, ENGINE_ERR_FILE when it does not open or holds no cue.
 */
LONG subtitles_load(struct subtitles *subtitles, CONST_STRPTR name)
{
	FILE *file = fopen((const char *)name, "r");
	if (file == NULL)
		return ENGINE_ERR_FILE;
	subtitles_flush(subtitles);
	char line[LINE_SIZE];
	VCTime start, end;
	while (fgets(line, sizeof(line), file) != NULL)
		if (times_read(line, &start, &end))
			cue_read(subtitles, file, start, end);
	fclose(file);
	return subtitles->cue_count != 0 ? 0 : ENGINE_ERR_FILE;
}
