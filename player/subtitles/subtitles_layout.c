/* SPDX-License-Identifier: MPL-2.0 */
/*
 * subtitles_layout.c - the cues of a time stacked at the picture's edges and
 * drawn into masks.
 *
 * The cues a time lies in stand in two stacks: most at the picture's foot,
 * those marked for the top at its top.  A stack is drawn once each time its
 * cues change.  A cue keeps its place in its stack while it is shown, so it
 * does not move when others come or go; a new one takes the place nearest
 * the edge where it fits.  Each cue's characters are measured, cut into
 * lines no wider than the picture allows, and drawn by subtitles_font.c as
 * coverage into two planes: the glyphs themselves, and the glyphs with their
 * outline, stroked by the outline's width.  The two become a mask of one byte
 * a pixel: where the outline plane covers half a pixel or more, the grey of
 * white text over a black outline; elsewhere nothing.  So the text's edge
 * against its outline is smooth and the outline's edge against the picture
 * is hard, which is what can be had without reading the picture.
 */
#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "subtitles_private.h"

#define FONT_ROWS_PART 18u  /* the font's size: the picture's rows divided by this */
#define FONT_SMALLEST 12u
#define FONT_LARGEST 120u
#define OUTLINE_PART 14u    /* the outline's width: the font's size divided by this, one pixel at least */
#define WIDTH_PERCENT 92u   /* the widest a line may be, of the picture's width */
#define LINES_MOST 6u       /* lines of one cue drawn */
#define STACK_PART 2u       /* a stack takes at most the frame's rows divided by this */
#define PIXEL_UNITS 64      /* a glyph's advance is in 1/64 pixel */
#define ROUNDING_SLACK 2u   /* pixels beyond the widest line: its width is cut to whole pixels, each pen rounded */
#define COVERED 128u        /* coverage from which a pixel belongs to the text */
#define GREY_DARKEST 1u     /* the mask's black: 0 says "not the text's" */

/* One line of a cue as it will be drawn */
struct line
{
	ULONG first; /* its first glyph */
	ULONG count; /* its glyphs */
	ULONG width; /* its pixels */
};

/* A cue laid out */
struct layout
{
	const struct glyph *glyphs;    /* its characters */
	struct line lines[LINES_MOST]; /* its lines */
	ULONG count;                   /* how many */
	ULONG widest;                  /* the widest one's pixels */
	ULONG height;                  /* the cue's rows, its outline's included */
};

/**
 * plane_blend() - Blend a rectangle of coverage into a plane: each pixel keeps the larger of the two.
 * @plane: the plane
 * @left:  where the rectangle's left edge goes; what falls outside the plane is cut off
 * @top:   and its top
 * @from:  the rectangle, a byte a pixel
 * @width: its pixels
 * @rows:  its rows
 * @pitch: from one of its rows to the next
 */
void plane_blend(struct plane *plane, LONG left, LONG top, const UBYTE *from, ULONG width, ULONG rows, LONG pitch)
{
	for (ULONG row = 0; row < rows; row++, from += pitch)
	{
		LONG y = top + (LONG)row;
		if (y < 0 || y >= (LONG)plane->height)
			continue;
		UBYTE *to = plane->bytes + (ULONG)y * plane->width;
		for (ULONG x = 0; x < width; x++)
		{
			LONG across = left + (LONG)x;
			if (across >= 0 && across < (LONG)plane->width && from[x] > to[across])
				to[across] = from[x];
		}
	}
}

/**
 * mark_take() - Take a mark of a cue's text.
 * @code:  the character
 * @style: the style the characters behind it are drawn in, changed by a style's mark
 *
 * MARK_TOP is taken too: the cue's edge is known from it already (cue.top).
 *
 * Return: TRUE when the character was a mark.
 */
static BOOL mark_take(ULONG code, UBYTE *style)
{
	switch (code)
	{
	case MARK_ITALIC_ON:
		*style |= STYLE_ITALIC;
		return TRUE;
	case MARK_ITALIC_OFF:
		*style &= (UBYTE)~STYLE_ITALIC;
		return TRUE;
	case MARK_BOLD_ON:
		*style |= STYLE_BOLD;
		return TRUE;
	case MARK_BOLD_OFF:
		*style &= (UBYTE)~STYLE_BOLD;
		return TRUE;
	case MARK_TOP:
		return TRUE;
	default:
		return FALSE;
	}
}

/**
 * glyphs_from_text() - Turn a cue's text into glyphs, each with its style and its advance.
 * @font:   the font, its size set
 * @text:   the cue's text, UTF-8, with its style marks
 * @glyphs: gets the characters; the marks are taken out and left as each one's style
 * @room:   how many it holds; the text is cut there
 *
 * Return: how many there are.
 */
static ULONG glyphs_from_text(struct font *font, const char *text, struct glyph *glyphs, ULONG room)
{
	ULONG count = 0, length = (ULONG)strlen(text);
	const UBYTE *from = (const UBYTE *)text;
	UBYTE style = 0;

	while (length != 0 && count < room)
	{
		struct glyph *glyph = &glyphs[count];
		ULONG used = utf8_next(from, length, &glyph->code);
		from += used;
		length -= used;
		if (mark_take(glyph->code, &style))
			continue;
		glyph->advance = 0;
		glyph->style = style;
		if (glyph->code != '\n')
			font_measure(font, glyph, count != 0 && glyph[-1].code != '\n' ? glyph - 1 : NULL);
		count++;
	}
	return count;
}

/**
 * line_fit() - How many of a cue's glyphs make its next line.
 * @glyphs: the glyphs left
 * @count:  how many
 * @most:   the widest the line may be, in 1/64 pixel
 * @width:  gets the line's width, in 1/64 pixel
 *
 * The line ends at a newline, or at the last space before the width runs
 * out; a first word too wide is taken whole.
 *
 * Return: the line's glyphs.
 */
static ULONG line_fit(const struct glyph *glyphs, ULONG count, LONG most, LONG *width)
{
	ULONG fits = 0, length = 0;
	LONG fits_width = 0;
	*width = 0;
	for (; length < count && glyphs[length].code != '\n'; length++)
	{
		if (glyphs[length].code == ' ')
		{
			fits = length;
			fits_width = *width;
		}
		*width += glyphs[length].advance;
		if (*width > most && fits != 0)
		{
			*width = fits_width;
			return fits;
		}
	}
	return length;
}

/**
 * line_cut() - Take the next line off a cue's glyphs, no wider than allowed.
 * @glyphs: the glyphs left
 * @count:  how many
 * @most:   the widest the line may be, in 1/64 pixel
 * @line:   gets the line's count and width
 *
 * Return: the glyphs used up: the line's and the spaces or the newline behind it.
 */
static ULONG line_cut(const struct glyph *glyphs, ULONG count, LONG most, struct line *line)
{
	LONG width;
	line->count = line_fit(glyphs, count, most, &width);
	line->width = (ULONG)(width / PIXEL_UNITS);
	ULONG used = line->count;
	while (used < count && glyphs[used].code == ' ')
		used++;
	if (used < count && glyphs[used].code == '\n')
		used++;
	return used;
}

/**
 * lines_cut() - Cut a cue's glyphs into lines.
 * @glyphs: the glyphs
 * @count:  how many
 * @most:   the widest a line may be, in pixels
 * @layout: gets the glyphs and the lines
 */
static void lines_cut(const struct glyph *glyphs, ULONG count, ULONG most, struct layout *layout)
{
	ULONG first = 0;
	layout->glyphs = glyphs;
	layout->count = 0;
	layout->widest = 0;
	while (first < count && layout->count < LINES_MOST)
	{
		struct line *line = &layout->lines[layout->count++];
		line->first = first;
		first += line_cut(glyphs + first, count - first, (LONG)most * PIXEL_UNITS, line);
		if (line->width > layout->widest)
			layout->widest = line->width;
	}
}

/**
 * lines_draw() - Draw a cue's lines into the two planes, each line centred.
 * @font:    the font, its size set
 * @layout:  the cue's lines
 * @outline: the outline's width
 * @top:     the planes' row where the cue's rows begin
 * @text_plane:    gets the glyphs
 * @outline_plane: gets the glyphs with their outline
 */
static void lines_draw(struct font *font, const struct layout *layout, ULONG outline, ULONG top, struct plane *text_plane,
                       struct plane *outline_plane)
{
	for (ULONG i = 0; i < layout->count; i++)
	{
		const struct line *line = &layout->lines[i];
		LONG pen_x = (LONG)(text_plane->width - line->width) / 2 * PIXEL_UNITS; /* where the next glyph goes: 1/64 pixel, the line centred */
		LONG baseline_row = (LONG)(top + outline + i * font_line_height(font) + font_ascent(font)); /* the row the letters sit on */
		for (ULONG j = 0; j < line->count; j++)
		{
			const struct glyph *glyph = &layout->glyphs[line->first + j];
			if (glyph->code != ' ')
				font_draw(font, glyph, (pen_x + PIXEL_UNITS / 2) / PIXEL_UNITS, baseline_row, text_plane, outline_plane);
			pen_x += glyph->advance;
		}
	}
}

/**
 * mask_make() - Make the mask of the two planes, in the outline plane's place.
 * @text_plane:    the glyphs' coverage
 * @outline_plane: the coverage of the glyphs with their outline; becomes the mask
 */
static void mask_make(const struct plane *text_plane, struct plane *outline_plane)
{
	ULONG pixels = text_plane->width * text_plane->height;
	for (ULONG i = 0; i < pixels; i++)
	{
		if (outline_plane->bytes[i] < COVERED)
			outline_plane->bytes[i] = 0;
		else
			outline_plane->bytes[i] = text_plane->bytes[i] < GREY_DARKEST ? GREY_DARKEST : text_plane->bytes[i];
	}
}

/**
 * font_size_for() - Set the font's size for a picture.
 * @subtitles: the subtitles
 * @rows:      the picture's rows
 * @outline:   gets the outline's width
 *
 * Return: TRUE, FALSE when there is no font to draw with.
 */
static BOOL font_size_for(struct subtitles *subtitles, ULONG rows, ULONG *outline)
{
	ULONG size = rows / FONT_ROWS_PART;
	size = size < FONT_SMALLEST ? FONT_SMALLEST : size > FONT_LARGEST ? FONT_LARGEST : size;
	*outline = size / OUTLINE_PART > 0 ? size / OUTLINE_PART : 1;
	return subtitles->font != NULL && font_set_size(subtitles->font, size, *outline);
}

/**
 * stack_layout() - Measure a stack's cues and cut them into lines.
 * @subtitles: the subtitles, their font's size set; gets the glyphs
 * @cues:      the cues
 * @count:     how many
 * @width:     the picture's pixels
 * @outline:   the outline's width
 * @layouts:   gets each cue's lines
 */
static void stack_layout(struct subtitles *subtitles, const struct cue *const *cues, ULONG count, ULONG width, ULONG outline,
                         struct layout *layouts)
{
	ULONG used = 0;
	ULONG line_height = font_line_height(subtitles->font);
	for (ULONG i = 0; i < count; i++)
	{
		struct layout *layout = &layouts[i];
		ULONG glyph_count = glyphs_from_text(subtitles->font, cues[i]->text, subtitles->glyphs + used, GLYPHS_MOST - used);
		lines_cut(subtitles->glyphs + used, glyph_count, width * WIDTH_PERCENT / 100, layout);
		layout->height = layout->count * line_height + 2 * outline;
		used += glyph_count;
	}
}

/**
 * kept_offset() - Where a cue stood in the stack drawn before.
 * @kept:   that stack's slots
 * @count:  how many
 * @serial: the cue's
 *
 * Return: its offset, NO_OFFSET for a cue new to the stack or not drawn in it.
 */
static ULONG kept_offset(const struct slot *kept, ULONG count, ULONG serial)
{
	for (ULONG i = 0; i < count; i++)
		if (kept[i].serial == serial)
			return kept[i].offset;
	return NO_OFFSET;
}

/**
 * rows_free() - Whether a band of rows of a stack is free.
 * @slots:  the stack's slots
 * @count:  how many
 * @offset: the first of the rows, from the stack's edge
 * @height: how many
 *
 * Return: TRUE when no cue placed stands there.
 */
static BOOL rows_free(const struct slot *slots, ULONG count, ULONG offset, ULONG height)
{
	for (ULONG i = 0; i < count; i++)
		if (slots[i].offset != NO_OFFSET && offset < slots[i].offset + slots[i].height && slots[i].offset < offset + height)
			return FALSE;
	return TRUE;
}

/**
 * slot_fit() - The place nearest a stack's edge where a cue fits among those placed.
 * @slots:      the stack's slots
 * @count:      how many
 * @height:     the cue's rows
 * @stack_rows: the rows the stack may take in all, from its edge
 *
 * Return: the offset, NO_OFFSET where it does not fit.
 */
static ULONG slot_fit(const struct slot *slots, ULONG count, ULONG height, ULONG stack_rows)
{
	ULONG best = rows_free(slots, count, 0, height) ? 0 : NO_OFFSET;
	for (ULONG i = 0; i < count; i++)
	{
		ULONG offset = slots[i].offset + slots[i].height; /* just past a cue placed */
		if (slots[i].offset != NO_OFFSET && offset < best && rows_free(slots, count, offset, height))
			best = offset;
	}
	return best != NO_OFFSET && best + height <= stack_rows ? best : NO_OFFSET;
}

/**
 * stack_size() - The planes' size for a stack's cues.
 * @stack:   the stack, its slots placed
 * @layouts: the cues' lines
 * @outline: the outline's width
 * @width:   the picture's pixels: the planes are no wider
 *
 * Return: a plane of that size, without bytes; its height 0 for a stack with no cue placed.
 */
static struct plane stack_size(const struct stack *stack, const struct layout *layouts, ULONG outline, ULONG width)
{
	struct plane size = {0};
	for (ULONG i = 0; i < stack->slot_count; i++)
	{
		const struct slot *slot = &stack->slots[i];
		if (slot->offset == NO_OFFSET)
			continue;
		ULONG slot_width = layouts[i].widest + 2 * outline + ROUNDING_SLACK;
		if (slot_width > size.width)
			size.width = slot_width;
		ULONG slot_bottom = slot->offset + slot->height;
		if (slot_bottom > size.height)
			size.height = slot_bottom;
	}
	if (size.width > width)
		size.width = width;
	return size;
}

/**
 * stack_draw() - Draw a stack's placed cues into its planes.
 * @subtitles: the subtitles
 * @stack:     the stack, its slots placed; gets the planes and their version
 * @edge:      its edge: the cues stand from the foot up, or from the top down
 * @layouts:   the cues' lines
 * @outline:   the outline's width
 * @width:     the picture's pixels
 *
 * Without memory there are no planes and the cues are not shown.
 */
static void stack_draw(struct subtitles *subtitles, struct stack *stack, enum edge edge, const struct layout *layouts, ULONG outline,
                       ULONG width)
{
	struct plane text_plane = stack_size(stack, layouts, outline, width);
	if (text_plane.height == 0)
		return;
	text_plane.bytes = AllocVec(2 * text_plane.width * text_plane.height, MEMF_ANY | MEMF_CLEAR); /* the two planes, one behind the other */
	if (text_plane.bytes == NULL)
		return;
	struct plane outline_plane = {
	    .bytes = text_plane.bytes + text_plane.width * text_plane.height,
	    .width = text_plane.width,
	    .height = text_plane.height,
	};
	for (ULONG i = 0; i < stack->slot_count; i++)
	{
		const struct slot *slot = &stack->slots[i];
		if (slot->offset == NO_OFFSET)
			continue;
		ULONG top = edge == EDGE_TOP ? slot->offset : text_plane.height - slot->offset - slot->height;
		lines_draw(subtitles->font, &layouts[i], outline, top, &text_plane, &outline_plane);
	}
	mask_make(&text_plane, &outline_plane);
	stack->text_plane = text_plane;
	stack->mask = outline_plane;
	stack->version = ++subtitles->last_version;
}

/**
 * stack_make() - Draw a stack anew with the cues of the time.
 * @subtitles: the subtitles
 * @stack:     the stack
 * @edge:      its edge
 * @cues:      its cues, by their start
 * @count:     how many
 * @width:     the picture's pixels
 * @rows:      the frame's rows
 * @resized:   the picture or the frame has another size: no cue keeps its place
 *
 * The cues the stack held before keep their places; the new ones take the
 * places nearest the edge where they fit, in the order they started.
 * Without a font or without memory the stack has no planes and its cues are
 * not shown.
 */
static void stack_make(struct subtitles *subtitles, struct stack *stack, enum edge edge, const struct cue *const *cues, ULONG count,
                       ULONG width, ULONG rows, BOOL resized)
{
	/* the places the cues held before, unless the size changed: those are gone */
	struct slot kept[SHOWN_MOST];
	ULONG kept_count = resized ? 0 : stack->slot_count;
	memcpy(kept, stack->slots, kept_count * sizeof(*kept));
	FreeVec(stack->text_plane.bytes);

	/* the stack anew: a slot for each cue of the time, not yet placed */
	*stack = (struct stack){.slot_count = count, .drawn_width = width, .drawn_rows = rows};
	for (ULONG i = 0; i < count; i++)
		stack->slots[i] = (struct slot){.serial = cues[i]->serial, .offset = NO_OFFSET};

	ULONG outline;
	if (count == 0 || !font_size_for(subtitles, rows, &outline))
		return;

	/* each cue's glyphs cut into lines at the font's size, which gives its height; a cue shown before keeps its place */
	struct layout layouts[SHOWN_MOST];
	stack_layout(subtitles, cues, count, width, outline, layouts);
	for (ULONG i = 0; i < count; i++)
	{
		stack->slots[i].height = layouts[i].height;
		stack->slots[i].offset = kept_offset(kept, kept_count, cues[i]->serial);
	}

	/* the new cues, in the order they started, take the free places nearest the edge, none past the stack's share of the frame */
	for (ULONG i = 0; i < count; i++)
		if (stack->slots[i].offset == NO_OFFSET)
			stack->slots[i].offset = slot_fit(stack->slots, count, stack->slots[i].height, rows / STACK_PART);

	/* the planes, and the mask the picture gets */
	stack_draw(subtitles, stack, edge, layouts, outline, width);
}

/**
 * stack_holds() - Whether a stack holds just these cues.
 * @stack: the stack
 * @cues:  the cues, by their start
 * @count: how many
 *
 * Return: TRUE if it does.
 */
static BOOL stack_holds(const struct stack *stack, const struct cue *const *cues, ULONG count)
{
	if (stack->slot_count != count)
		return FALSE;
	for (ULONG i = 0; i < count; i++)
		if (stack->slots[i].serial != cues[i]->serial)
			return FALSE;
	return TRUE;
}

/**
 * stack_update() - Draw a stack anew when its cues or the frame's size changed.
 * @subtitles: the subtitles
 * @edge:      the stack's edge
 * @shown:     the cues of the time, by their start
 * @count:     how many
 * @frame:     the frame
 */
void stack_update(struct subtitles *subtitles, enum edge edge, const struct cue *const *shown, ULONG count,
                         const struct VCFrame *frame)
{
	const struct cue *cues[SHOWN_MOST];
	ULONG edge_count = 0;
	for (ULONG i = 0; i < count; i++)
		if (shown[i]->top == (edge == EDGE_TOP))
			cues[edge_count++] = shown[i];

	struct stack *stack = &subtitles->stacks[edge];
	BOOL resized = stack->drawn_width != (ULONG)frame->Crop.Width || stack->drawn_rows != frame->Height;
	if (resized || !stack_holds(stack, cues, edge_count))
		stack_make(subtitles, stack, edge, cues, edge_count, (ULONG)frame->Crop.Width, frame->Height, resized);
}
