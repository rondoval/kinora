/* SPDX-License-Identifier: MPL-2.0 */
/*
 * subtitles_panel.c - lines of plain text in a dark box in the picture's
 * top left corner: the statistics.
 *
 * The text changes often, so its characters are rendered once for a size
 * and kept; a new text is put together from those.  The panel is a mask as
 * the subtitles' is, with every pixel of the box in it, and is written into
 * each picture whole.
 */
#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "subtitles_private.h"

#define SIZE_ROWS_PART 45u /* the font's size: the picture's rows divided by this */
#define SIZE_SMALLEST 10u
#define SIZE_LARGEST 28u
#define BOX_GREY 28        /* the box behind the text; not 0, which would leave the picture */
#define BOX_PART 2u        /* the gap around the text and from the picture's edge: the font's size divided by this */

/**
 * panel_free() - Free a panel's glyphs and mask.
 * @panel: the panel
 */
void panel_free(struct panel *panel)
{
	for (ULONG i = 0; i < PANEL_GLYPHS; i++)
		FreeVec(panel->glyphs[i].bytes);
	FreeVec(panel->mask);
	memset(panel, 0, sizeof(*panel));
}

/**
 * panel_glyphs_render() - Render the panel's characters at a size.
 * @subtitles: the subtitles, with a font
 * @size:      the size, in pixels
 *
 * Return: TRUE, FALSE when the font has no such size; a character that does not render stays empty.
 */
static BOOL panel_glyphs_render(struct subtitles *subtitles, ULONG size)
{
	struct panel *panel = &subtitles->panel;
	panel_free(panel);
	if (!font_set_size(subtitles->font, size, 1))
		return FALSE;
	for (ULONG i = 0; i < PANEL_GLYPHS; i++)
		font_render(subtitles->font, PANEL_FIRST + i, &panel->glyphs[i]);
	panel->font_size = size;
	panel->line_height = font_line_height(subtitles->font);
	panel->ascent = font_ascent(subtitles->font);
	return TRUE;
}

/**
 * panel_glyph() - The rendered image of a character.
 * @panel:     the panel
 * @character: the character
 *
 * Return: its image, NULL for a character the panel cannot show.
 */
static const struct glyph_image *panel_glyph(const struct panel *panel, char character)
{
	return character >= PANEL_FIRST && character <= PANEL_LAST ? &panel->glyphs[character - PANEL_FIRST] : NULL;
}

/**
 * panel_gap() - The empty pixels around the text in its box, and between the box and the picture's corner.
 * @panel: the panel, its glyphs made
 *
 * Return: the pixels.
 */
static ULONG panel_gap(const struct panel *panel)
{
	return panel->font_size / BOX_PART;
}

/**
 * panel_measure() - How large a text is.
 * @panel: the panel, its glyphs made
 * @text:  the text
 * @lines: gets its lines
 *
 * Return: the pixels of its widest line.
 */
static ULONG panel_measure(const struct panel *panel, const char *text, ULONG *lines)
{
	ULONG widest = 0;
	ULONG width = 0;
	*lines = text[0] != 0 ? 1 : 0;
	for (; *text != 0; text++)
	{
		const struct glyph_image *glyph = panel_glyph(panel, *text);
		if (*text == '\n' && text[1] != 0)
		{
			(*lines)++;
			width = 0;
		}
		if (glyph != NULL)
			width += (ULONG)glyph->advance;
		if (width > widest)
			widest = width;
	}
	return widest;
}

/**
 * panel_draw_text() - Draw a text into the panel's mask.
 * @panel: the panel, its glyphs made
 * @mask:  the mask
 * @text:  the text
 * @gap:   the empty pixels between the box's edges and the text: panel_gap()
 *
 * Each character is drawn where the pen stands, and the pen then moves on by
 * its advance; a newline puts the pen back at the left and the base a line down.
 */
static void panel_draw_text(const struct panel *panel, struct plane *mask, const char *text, ULONG gap)
{
	LONG pen = (LONG)gap;                    /* the column of the next character's origin on its line */
	LONG base = (LONG)(gap + panel->ascent); /* the row of the line's base */
	for (; *text != 0; text++)
	{
		const struct glyph_image *glyph = panel_glyph(panel, *text);
		if (*text == '\n')
		{
			pen = (LONG)gap;
			base += (LONG)panel->line_height;
		}
		if (glyph == NULL)
			continue;
		if (glyph->bytes != NULL)
			plane_blend(mask, pen + glyph->left, base - glyph->top, glyph->bytes, glyph->width, glyph->rows, (LONG)glyph->width);
		pen += glyph->advance;
	}
}

/**
 * panel_make() - Make the panel's mask for a text and a picture.
 * @subtitles: the subtitles, with a font
 * @text:      the text
 * @frame:     the picture
 *
 * Without memory, or with a text larger than the picture, there is no mask.
 */
static void panel_make(struct subtitles *subtitles, const char *text, const struct VCFrame *frame)
{
	struct panel *panel = &subtitles->panel;
	ULONG rows = (ULONG)frame->Crop.Height;
	ULONG size = rows / SIZE_ROWS_PART;
	size = size < SIZE_SMALLEST ? SIZE_SMALLEST : size > SIZE_LARGEST ? SIZE_LARGEST : size;
	if (panel->font_size != size && !panel_glyphs_render(subtitles, size))
		return;
	FreeVec(panel->mask);
	panel->mask = NULL;

	ULONG lines;
	ULONG gap = panel_gap(panel);
	struct plane mask = {.width = panel_measure(panel, text, &lines) + 2 * gap};
	mask.height = lines * panel->line_height + 2 * gap;
	if (lines == 0 || mask.width + gap > (ULONG)frame->Crop.Width || mask.height + gap > rows)
		return;

	mask.bytes = AllocVec(mask.width * mask.height, MEMF_ANY);
	if (mask.bytes == NULL)
		return;
	memset(mask.bytes, BOX_GREY, mask.width * mask.height);
	panel_draw_text(panel, &mask, text, gap);
	panel->mask = mask.bytes;
	panel->mask_width = mask.width;
	panel->mask_height = mask.height;
}

/**
 * subtitles_draw_panel() - Write lines of text into a picture's top left corner, in a dark box.
 * @subtitles: the subtitles, whose font draws it; NULL does nothing
 * @text:      the lines, separated by newlines: the characters from space to tilde show
 * @version:   a number that changes when the text does
 * @frame:     the picture, about to be shown
 * @format:    VCF_* of its pixels
 */
void subtitles_draw_panel(struct subtitles *subtitles, const char *text, ULONG version, const struct VCFrame *frame, ULONG format)
{
	ULONG bytes = pixel_bytes(format);
	if (subtitles == NULL || subtitles->font == NULL || bytes == 0)
		return;

	struct panel *panel = &subtitles->panel;
	if (panel->mask == NULL || panel->text_version != version || panel->frame_rows != (ULONG)frame->Crop.Height)
	{
		panel_make(subtitles, text, frame);
		panel->text_version = version;
		panel->frame_rows = (ULONG)frame->Crop.Height;
	}
	if (panel->mask == NULL)
		return;
	struct plane mask = {.bytes = panel->mask, .width = panel->mask_width, .height = panel->mask_height};
	ULONG gap = panel_gap(panel);
	plane_put(&mask, frame, bytes, (ULONG)frame->Crop.X + gap, (ULONG)frame->Crop.Y + gap, TRUE);
}
