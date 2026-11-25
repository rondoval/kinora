/* SPDX-License-Identifier: MPL-2.0 */
/*
 * subtitles_private.h - the subtitles' own structures, and what their files
 * give each other.
 */
#ifndef KINORA_SUBTITLES_PRIVATE_H
#define KINORA_SUBTITLES_PRIVATE_H

#include "subtitles.h"

struct font; /* a font file open with FreeType; subtitles_font.c's own */

#define SHOWN_MOST 8u    /* cues shown at once; of more, those that started last */
#define GLYPHS_MOST 1024u /* the characters of one stack's cues */

/* One cue: a text and when it is shown */
struct cue
{
	VCTime start; /* from this time of the film */
	VCTime end;   /* to this one */
	VCTime reach; /* the latest end of this cue and of those before it */
	ULONG serial; /* its number, which stays when the cue moves in the array */
	BOOL top;     /* it stands at the picture's top: its text holds MARK_TOP */
	char *text;   /* plain UTF-8 text, lines separated by newlines, MARK_* among them */
};

#define STYLE_ITALIC 1u /* slanted */
#define STYLE_BOLD 2u   /* made fat */

/* One character of a cue as it will be drawn */
struct glyph
{
	ULONG code;   /* the character */
	ULONG index;  /* its glyph in the font */
	LONG advance; /* how far the next one stands to its right, in 1/64 pixel */
	UBYTE style;  /* STYLE_* it is drawn in */
};

/* Coverage, a byte a pixel: 0 for none, 255 for all of the pixel */
struct plane
{
	UBYTE *bytes; /* the pixels, row after row */
	ULONG width;  /* pixels in a row */
	ULONG height; /* rows */
};

#define PANEL_FIRST ' '  /* the first character a panel can show */
#define PANEL_LAST '~'   /* and the last */
#define PANEL_GLYPHS (PANEL_LAST - PANEL_FIRST + 1)

/* A glyph rendered once, to be drawn many times */
struct glyph_image
{
	UBYTE *bytes; /* its coverage, a byte a pixel, row after row; NULL for a glyph without ink */
	ULONG width;  /* pixels in a row */
	ULONG rows;   /* rows */
	LONG left;    /* from the pen to its left edge */
	LONG top;     /* from the line's base up to its top row */
	LONG advance; /* how far it moves the pen, in pixels */
};

/* A panel of text in the picture's corner */
struct panel
{
	struct glyph_image glyphs[PANEL_GLYPHS]; /* its characters, rendered at size */
	ULONG font_size;    /* the font size they were rendered at, 0 for none yet */
	ULONG line_height;  /* from one line's base to the next */
	ULONG ascent;       /* from a line's top to its base */
	UBYTE *mask;        /* the panel as a mask, as subtitles' is */
	ULONG mask_width;   /* its pixels */
	ULONG mask_height;  /* its rows */
	ULONG text_version; /* the text's version the mask shows */
	ULONG frame_rows;   /* the picture's rows it was made for */
};

/* Where the cues stand */
enum edge
{
	EDGE_BOTTOM, /* at the picture's foot: most cues */
	EDGE_TOP,    /* at its top: the cues with MARK_TOP */
	EDGES,       /* the last: the edges before it index the arrays */
};

/* One cue of a stack: where it stands, kept while it is shown */
struct slot
{
	ULONG serial; /* the cue's */
	ULONG offset; /* from the stack's edge to the cue's near side, in rows; NO_OFFSET for not drawn */
	ULONG height; /* the cue's rows */
};

#define NO_OFFSET 0xffffffffu /* slot.offset: the cue did not fit in the stack */

/* The cues shown at one edge, drawn together */
struct stack
{
	struct slot slots[SHOWN_MOST]; /* the cues, by their start */
	ULONG slot_count;              /* how many */
	ULONG version;                 /* the drawing's number, 0 for none; the canvases compare it */
	ULONG drawn_width;             /* the picture width it was drawn for, 0 to draw it anew */
	ULONG drawn_rows;              /* and the frame's rows */
	struct plane text_plane;       /* the cues' glyphs as coverage; its bytes NULL for none.  Written whole where the frame is black */
	struct plane mask;             /* behind it in the same memory: 0 leaves the picture's pixel, else the grey to write there */
};

#define CANVASES 8u /* the frames whose state is kept: the decoder has four of its own, a screen four parts */

/* What one stack left in a frame */
struct written
{
	ULONG version;      /* the stack's drawing, 0 for none */
	struct VCRect rect; /* where; Height 0 for nowhere */
	BOOL beside;        /* all of it lies beside the picture and is still there */
};

/* One of the decoder's frames, and what the stacks left in it */
struct canvas
{
	APTR data;                     /* the frame's buffer */
	struct written written[EDGES]; /* each edge's */
};

/* The subtitles of one film */
struct subtitles
{
	struct cue *cues;             /* the cues by their start; those of one start in the order they came */
	ULONG cue_count;              /* how many */
	ULONG cue_room;               /* how many the array holds */
	ULONG last_serial;            /* the number the last cue added got */

	struct font *font;            /* the font the cues are drawn with, NULL for none */
	struct stack stacks[EDGES];   /* the cues shown, at each edge */
	ULONG last_version;           /* the number the last drawing of a stack got */
	struct canvas canvases[CANVASES]; /* the frames written into since the decoder's frames were made */
	ULONG canvas_count;           /* how many */
	struct glyph glyphs[GLYPHS_MOST]; /* the characters of the stack being drawn */
	struct panel panel;           /* the statistics' panel */
};

/* subtitles_font.c */
struct font *font_open(CONST_STRPTR name);
void font_close(struct font *font);
BOOL font_set_size(struct font *font, ULONG size, ULONG outline);
ULONG font_line_height(const struct font *font);
ULONG font_ascent(const struct font *font);
void font_measure(struct font *font, struct glyph *glyph, const struct glyph *before);
BOOL font_render(struct font *font, ULONG code, struct glyph_image *image);
void font_draw(struct font *font, const struct glyph *drawn, LONG left, LONG base, struct plane *text_plane, struct plane *outline_plane);

/* subtitles_text.c */
ULONG utf8_next(const UBYTE *from, ULONG length, ULONG *code);
void text_plain(char *plain, ULONG room, const char *from, ULONG length, ULONG codec);

/* subtitles_cues.c */
ULONG cues_at(const struct subtitles *subtitles, VCTime time, const struct cue **shown, ULONG room);

/* subtitles_layout.c */
void plane_blend(struct plane *plane, LONG left, LONG top, const UBYTE *from, ULONG width, ULONG rows, LONG pitch);
void stack_update(struct subtitles *subtitles, enum edge edge, const struct cue *const *shown, ULONG count,
                  const struct VCFrame *frame);

/* subtitles_write.c */
ULONG pixel_bytes(ULONG format);
void plane_put(const struct plane *plane, const struct VCFrame *frame, ULONG bytes, ULONG left, ULONG top, BOOL whole);

/* subtitles_panel.c */
void panel_free(struct panel *panel);

#endif /* KINORA_SUBTITLES_PRIVATE_H */
