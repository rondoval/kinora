/* SPDX-License-Identifier: MPL-2.0 */
/*
 * subtitles.h - the film's subtitles: lines of text with the time they are
 * shown, from a stream of the file or from a SubRip file beside it, drawn
 * into each picture before it is shown.
 *
 *   subtitles.c         the subtitles made and freed, their font, the frames they are written into made anew
 *   subtitles_text.c    a cue's text made plain: sound UTF-8, its markup turned into marks or dropped
 *   subtitles_cues.c    the cues: from packets, from a SubRip file, those of a time found
 *   subtitles_font.c    a font file opened with FreeType, its glyphs measured and drawn
 *   subtitles_layout.c  the cues of a time stacked at the picture's edges and drawn into masks
 *   subtitles_write.c   the masks written into each picture's frame, and what each stack left in each frame
 *   subtitles_panel.c   lines of plain text in a dark box in the picture's corner: the statistics
 *
 * All of it runs in the engine's task.  The text is UTF-8 from the file to
 * the glyphs; what shows is what the font has glyphs for.  Characters are
 * placed one after another from the left: scripts that need shaping or run
 * from the right come out wrong.
 */
#ifndef KINORA_SUBTITLES_H
#define KINORA_SUBTITLES_H

#include <exec/types.h>
#include <libraries/videocodec.h>

#define SUBTITLE_TEXT_SIZE 512 /* a cue's plain text, with its NUL; longer ones are cut */

/* How the cues look (subtitles_set_look()) */
#define SUBTITLE_SIZE_NORMAL 100u   /* the font's size in percent: 100 is a frame's rows divided by 18 */
#define SUBTITLE_SIZE_LEAST 50u
#define SUBTITLE_SIZE_MOST 200u
#define SUBTITLE_DISTANCE_NORMAL 4u /* the gap between a stack and the frame's edge, in percent of the frame's rows; halved in a bar */
#define SUBTITLE_DISTANCE_MOST 30u

/* Marks in a cue's plain text: each changes how the characters behind it are drawn, or where the cue stands */
#define MARK_ITALIC_ON '\001'
#define MARK_ITALIC_OFF '\002'
#define MARK_BOLD_ON '\003'
#define MARK_BOLD_OFF '\004'
#define MARK_TOP '\005'  /* the cue stands at the picture's top */
#define MARK_LAST MARK_TOP

struct subtitles; /* the subtitles of one film; their own */
struct DMXPacket; /* <libraries/demuxer.h>: left out here, its struct DMXChapter has a field the ReAction macros' End would take */

struct subtitles *subtitles_open(void);
void subtitles_close(struct subtitles *subtitles);
void subtitles_flush(struct subtitles *subtitles);
void subtitles_add_packet(struct subtitles *subtitles, ULONG codec, const struct DMXPacket *packet);
LONG subtitles_load(struct subtitles *subtitles, CONST_STRPTR name);
LONG subtitles_set_font(struct subtitles *subtitles, CONST_STRPTR name);
void subtitles_set_look(struct subtitles *subtitles, ULONG size, ULONG distance);
void subtitles_frames_new(struct subtitles *subtitles);
void subtitles_draw(struct subtitles *subtitles, VCTime time, const struct VCFrame *frame, ULONG format);
void subtitles_draw_panel(struct subtitles *subtitles, const char *text, ULONG version, const struct VCFrame *frame, ULONG format);

#endif /* KINORA_SUBTITLES_H */
