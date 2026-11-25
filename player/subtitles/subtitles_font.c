/* SPDX-License-Identifier: MPL-2.0 */
/*
 * subtitles_font.c - a font file open with FreeType: its glyphs measured,
 * and drawn as coverage into two planes, the glyph itself and the glyph with
 * its outline, stroked by the outline's width.
 *
 * The file is read whole into memory and FreeType works from there.
 * FreeType's memory comes from exec.library, not from libnix's malloc(),
 * which the player's two tasks could not share.  Glyphs are loaded
 * unhinted: at the sizes subtitles have, hinting changes little, and the
 * outline is stroked from the same contour the text is filled from.
 */
#include <string.h>
#include <dos/dos.h>
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include FT_GLYPH_H
#include FT_MODULE_H
#include FT_OUTLINE_H
#include FT_STROKER_H

#include "subtitles_private.h"

#define PIXEL_UNITS 64            /* FreeType's 26.6 fixed point: units in a pixel */
#define ADVANCE_TO_UNITS_SHIFT 10 /* FT_Get_Advance() gives 16.16 */
#define LOAD_FLAGS (FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)
#define ITALIC_SLANT 0x3600       /* 16.16: how far a slanted glyph leans to the right for each unit up, about 0.21 */
#define BOLD_PART 20              /* a bold glyph is made fat by the font's size divided by this */
#define FIXED_ONE 0x10000         /* 16.16: 1.0 */

/* A font */
struct font
{
	struct FT_MemoryRec_ memory; /* FreeType's memory calls */
	FT_Library library;          /* FreeType */
	FT_Face face;                /* the font */
	FT_Stroker stroker;          /* what makes a glyph's outline */
	UBYTE *file;                 /* the font file's bytes, which the face reads from */
	ULONG size;                  /* the size set, in pixels; 0 for none yet */
	ULONG outline;               /* the outline's width set, in pixels */
};

/**
 * memory_alloc() - FreeType's allocator.
 * @memory: unused
 * @size:   the bytes
 *
 * Return: the block, NULL without memory.
 */
static void *memory_alloc(FT_Memory memory, long size)
{
	(void)memory;
	return AllocVec((ULONG)size, MEMF_ANY);
}

/**
 * memory_free() - FreeType's free().
 * @memory: unused
 * @block:  the block
 */
static void memory_free(FT_Memory memory, void *block)
{
	(void)memory;
	FreeVec(block);
}

/**
 * memory_realloc() - FreeType's realloc().
 * @memory:  FreeType's memory calls
 * @current: the block's bytes
 * @wanted:  the bytes it shall have
 * @block:   the block
 *
 * Return: the new block, NULL without memory, the old one then kept.
 */
static void *memory_realloc(FT_Memory memory, long current, long wanted, void *block)
{
	void *grown = memory_alloc(memory, wanted);
	if (grown == NULL)
		return NULL;
	memcpy(grown, block, (size_t)(current < wanted ? current : wanted));
	FreeVec(block);
	return grown;
}

/**
 * file_read() - Read a file whole into memory.
 * @name:   the file
 * @length: gets its bytes
 *
 * Return: the bytes, for FreeVec(); NULL when the file does not open, is empty or does not fit the memory.
 */
static UBYTE *file_read(CONST_STRPTR name, LONG *length)
{
	BPTR file = Open(name, MODE_OLDFILE);
	if (file == 0)
		return NULL;
	UBYTE *bytes = NULL;
	Seek(file, 0, OFFSET_END);
	*length = Seek(file, 0, OFFSET_BEGINNING);
	if (*length > 0)
		bytes = AllocVec((ULONG)*length, MEMF_ANY);
	if (bytes != NULL && Read(file, bytes, *length) != *length)
	{
		FreeVec(bytes);
		bytes = NULL;
	}
	Close(file);
	return bytes;
}

/**
 * font_open() - Open a font file.
 * @name: the file: TrueType or OpenType
 *
 * Return: the font, NULL when the file is none FreeType reads, or without memory.
 */
struct font *font_open(CONST_STRPTR name)
{
	struct font *font = AllocVec(sizeof(*font), MEMF_ANY | MEMF_CLEAR);
	if (font == NULL)
		return NULL;
	font->memory = (struct FT_MemoryRec_){.alloc = memory_alloc, .free = memory_free, .realloc = memory_realloc};
	LONG length = 0;
	font->file = file_read(name, &length);
	BOOL made = font->file != NULL && FT_New_Library(&font->memory, &font->library) == 0;
	if (made)
	{
		FT_Add_Default_Modules(font->library);
		made = FT_New_Memory_Face(font->library, font->file, length, 0, &font->face) == 0 &&
		       FT_Stroker_New(font->library, &font->stroker) == 0;
	}
	if (!made)
	{
		font_close(font);
		return NULL;
	}
	return font;
}

/**
 * font_close() - Close a font.
 * @font: the font, whole or half made; NULL does nothing
 */
void font_close(struct font *font)
{
	if (font == NULL)
		return;
	if (font->stroker != NULL)
		FT_Stroker_Done(font->stroker);
	if (font->face != NULL)
		FT_Done_Face(font->face);
	if (font->library != NULL)
		FT_Done_Library(font->library);
	FreeVec(font->file);
	FreeVec(font);
}

/**
 * font_set_size() - Set the size glyphs are measured and drawn at.
 * @font:    the font
 * @size:    the size, in pixels
 * @outline: the outline's width, in pixels
 *
 * Return: TRUE, FALSE when the font has no such size.
 */
BOOL font_set_size(struct font *font, ULONG size, ULONG outline)
{
	if (font->size == size && font->outline == outline)
		return TRUE;
	font->size = 0;
	if (FT_Set_Pixel_Sizes(font->face, 0, size) != 0)
		return FALSE;
	FT_Stroker_Set(font->stroker, (FT_Fixed)(outline * PIXEL_UNITS), FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);
	font->size = size;
	font->outline = outline;
	return TRUE;
}

/**
 * font_line_height() - How far a line's base is below the base of the line above.
 * @font: the font, its size set
 *
 * Return: the rows.
 */
ULONG font_line_height(const struct font *font)
{
	return (ULONG)(font->face->size->metrics.height / PIXEL_UNITS);
}

/**
 * font_ascent() - How far a line's base is below its top.
 * @font: the font, its size set
 *
 * Return: the rows.
 */
ULONG font_ascent(const struct font *font)
{
	return (ULONG)(font->face->size->metrics.ascender / PIXEL_UNITS);
}

/**
 * font_measure() - Find a character's glyph and how far it moves the pen.
 * @font:   the font, its size set
 * @glyph:  the character: its code and style set, its index and advance filled in
 * @before: the glyph to its left on the line, NULL for none: the font's kerning between the two is added to its advance
 */
void font_measure(struct font *font, struct glyph *glyph, const struct glyph *before)
{
	FT_Fixed advance = 0;
	glyph->index = FT_Get_Char_Index(font->face, glyph->code);
	FT_Get_Advance(font->face, glyph->index, LOAD_FLAGS, &advance);
	glyph->advance = (LONG)(advance >> ADVANCE_TO_UNITS_SHIFT);

	FT_Vector kerning = {0, 0};
	if (before != NULL && FT_HAS_KERNING(font->face))
		FT_Get_Kerning(font->face, before->index, glyph->index, FT_KERNING_UNFITTED, &kerning);
	glyph->advance += (LONG)kerning.x;
	if (glyph->style & STYLE_BOLD)
		glyph->advance += (LONG)(font->size * PIXEL_UNITS / BOLD_PART);
}

/**
 * glyph_style() - Slant a glyph, make it fat, or both.
 * @font:  the font
 * @glyph: the glyph, a contour
 * @style: STYLE_*
 */
static void glyph_style(const struct font *font, FT_Glyph glyph, UBYTE style)
{
	if (glyph->format != FT_GLYPH_FORMAT_OUTLINE)
		return;
	if (style & STYLE_BOLD)
		FT_Outline_Embolden(&((FT_OutlineGlyph)glyph)->outline, (FT_Pos)(font->size * PIXEL_UNITS / BOLD_PART));
	if (style & STYLE_ITALIC)
	{
		FT_Matrix slant = {.xx = FIXED_ONE, .xy = ITALIC_SLANT, .yx = 0, .yy = FIXED_ONE};
		FT_Glyph_Transform(glyph, &slant, NULL);
	}
}

/**
 * glyph_blend() - Render a glyph and blend its coverage into planes.
 * @glyph: the glyph, an outline; it stays as it is
 * @left:  the pen's place in the planes
 * @base:  the row of the line's base
 * @first: a plane to blend into
 * @other: another, NULL for none
 */
static void glyph_blend(FT_Glyph glyph, LONG left, LONG base, struct plane *first, struct plane *other)
{
	FT_Glyph rendered = glyph;
	if (FT_Glyph_To_Bitmap(&rendered, FT_RENDER_MODE_NORMAL, NULL, 0) != 0)
		return;
	const FT_BitmapGlyph image = (FT_BitmapGlyph)rendered;
	const FT_Bitmap *bitmap = &image->bitmap;
	plane_blend(first, left + image->left, base - image->top, bitmap->buffer, bitmap->width, bitmap->rows, bitmap->pitch);
	if (other != NULL)
		plane_blend(other, left + image->left, base - image->top, bitmap->buffer, bitmap->width, bitmap->rows, bitmap->pitch);
	FT_Done_Glyph(rendered);
}

/**
 * font_draw() - Draw a glyph into the planes.
 * @font:  the font, its size set
 * @drawn: the glyph: its index and style
 * @left:  the pen's place in the planes
 * @base:  the row of the line's base
 * @text_plane:    gets the glyph
 * @outline_plane: gets the glyph and its outline
 *
 * A glyph that does not load, or is no contour, is left out.
 */
void font_draw(struct font *font, const struct glyph *drawn, LONG left, LONG base, struct plane *text_plane, struct plane *outline_plane)
{
	FT_Glyph glyph;
	if (FT_Load_Glyph(font->face, drawn->index, LOAD_FLAGS) != 0 || FT_Get_Glyph(font->face->glyph, &glyph) != 0)
		return;
	glyph_style(font, glyph, drawn->style);
	glyph_blend(glyph, left, base, text_plane, outline_plane);
	if (FT_Glyph_Stroke(&glyph, font->stroker, 1) == 0)
		glyph_blend(glyph, left, base, outline_plane, NULL);
	FT_Done_Glyph(glyph);
}

/**
 * font_render() - Render a character once, to be drawn many times.
 * @font:    the font, its size set
 * @code:    the character
 * @image:   gets its coverage, for FreeVec(), and where it stands to the pen
 *
 * Return: TRUE, FALSE when the glyph does not load or without memory.
 */
BOOL font_render(struct font *font, ULONG code, struct glyph_image *image)
{
	if (FT_Load_Char(font->face, code, LOAD_FLAGS | FT_LOAD_RENDER) != 0)
		return FALSE;
	const FT_GlyphSlot slot = font->face->glyph;
	const FT_Bitmap *bitmap = &slot->bitmap;
	*image = (struct glyph_image){.width = bitmap->width,
	                              .rows = bitmap->rows,
	                              .left = slot->bitmap_left,
	                              .top = slot->bitmap_top,
	                              .advance = (LONG)((slot->advance.x + PIXEL_UNITS / 2) / PIXEL_UNITS)};
	if (bitmap->width == 0 || bitmap->rows == 0)
		return TRUE;
	image->bytes = AllocVec(bitmap->width * bitmap->rows, MEMF_ANY);
	if (image->bytes == NULL)
		return FALSE;
	for (ULONG row = 0; row < bitmap->rows; row++)
		memcpy(image->bytes + row * bitmap->width, bitmap->buffer + (LONG)row * bitmap->pitch, bitmap->width);
	return TRUE;
}
