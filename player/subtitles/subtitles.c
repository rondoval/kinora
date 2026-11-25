/* SPDX-License-Identifier: MPL-2.0 */
/*
 * subtitles.c - the subtitles of a film made and freed, the font they are
 * drawn with, and the frames they are written into made anew.
 */
#include <exec/memory.h>
#include <proto/exec.h>

#include "subtitles_private.h"

#define ENGINE_ERR_FILE (-2) /* as engine.h's: the file did not open or is no font */

/**
 * subtitles_open() - Make the subtitles of a film, with no cues and no font.
 *
 * Return: the subtitles, NULL without memory.
 */
struct subtitles *subtitles_open(void)
{
	return AllocVec(sizeof(struct subtitles), MEMF_ANY | MEMF_CLEAR);
}

/**
 * subtitles_close() - Free the subtitles.
 * @subtitles: the subtitles; NULL does nothing
 */
void subtitles_close(struct subtitles *subtitles)
{
	if (subtitles == NULL)
		return;
	subtitles_flush(subtitles);
	FreeVec(subtitles->cues);
	for (enum edge edge = EDGE_BOTTOM; edge < EDGES; edge++)
		FreeVec(subtitles->stacks[edge].text_plane.bytes);
	panel_free(&subtitles->panel);
	font_close(subtitles->font);
	FreeVec(subtitles);
}

/**
 * subtitles_set_font() - Draw the cues with another font.
 * @subtitles: the subtitles
 * @name:      the font file: TrueType or OpenType
 *
 * Return: 0, ENGINE_ERR_FILE when the file is no font; the font before then stays.
 */
LONG subtitles_set_font(struct subtitles *subtitles, CONST_STRPTR name)
{
	struct font *font = font_open(name);
	if (font == NULL)
		return ENGINE_ERR_FILE;
	font_close(subtitles->font);
	panel_free(&subtitles->panel);
	subtitles->font = font;
	for (enum edge edge = EDGE_BOTTOM; edge < EDGES; edge++)
		subtitles->stacks[edge].drawn_width = 0; /* drawn anew with the new font */
	return 0;
}

/**
 * subtitles_frames_new() - Forget what was written into the decoder's frames: it has new ones.
 * @subtitles: the subtitles; NULL does nothing
 *
 * The display makes each new frame's bars black when it first comes, so
 * nothing a stack wrote there before is left.
 */
void subtitles_frames_new(struct subtitles *subtitles)
{
	if (subtitles != NULL)
		subtitles->canvas_count = 0;
}
