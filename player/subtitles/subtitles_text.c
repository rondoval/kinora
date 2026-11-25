/* SPDX-License-Identifier: MPL-2.0 */
/*
 * subtitles_text.c - a cue's text made plain: sound UTF-8, the markup of
 * SubRip (<i>...</i>) and ASS ({\i1}, {\an8}, \N) turned into marks where it
 * says italic, bold or "at the top", and dropped where it says anything else.
 */
#include <libraries/demuxer.h>

#include "subtitles_private.h"

#define ASS_FIELDS_BEFORE_TEXT 8u /* the commas before an ASS event's text in a Matroska packet */
#define MOVTEXT_LENGTH_BYTES 2u   /* an MP4 timed text sample starts with its text's length */
#define UTF8_BYTES_MOST 4u        /* the longest a character is in UTF-8 */
#define ASCII_LOWER_CASE 0x20u    /* set in an ASCII letter makes it lower case; '/' has it already */

/*
 * UTF-8: a character is a lead byte and up to three follow bytes.  The lead
 * starts with as many ones as the sequence has bytes, then a zero (none for
 * one byte alone); each follow byte is 10xxxxxx.  The character's bits fill
 * the x's, the highest in the lead.
 */
#define UTF8_FOLLOW_BITS 6u   /* the character's bits in a follow byte */
#define UTF8_FOLLOW_MASK 0x3fu /* which bits of it they are */
#define UTF8_FOLLOW_TAG 0x80u  /* the 10 a follow byte starts with */
#define UTF8_TAG_MASK 0xc0u    /* the two bits that hold it */

/* The lead's ones, by the follow bytes behind it: 0xxxxxxx, 110xxxxx, 1110xxxx, 11110xxx */
static const UBYTE utf8_leads[UTF8_BYTES_MOST] = {0x00, 0xc0, 0xe0, 0xf0};

/* The first character that takes each number of follow bytes */
static const ULONG utf8_firsts[UTF8_BYTES_MOST] = {0x0, 0x80, 0x800, 0x10000};

/**
 * utf8_next() - Take the next character of UTF-8 text.
 * @from:   the text
 * @length: its bytes, at least one
 * @code:   gets the character
 *
 * Return: the bytes taken; a broken sequence is taken as one byte of Latin-1.
 */
ULONG utf8_next(const UBYTE *from, ULONG length, ULONG *code)
{
	UBYTE first = from[0];
	ULONG more = UTF8_BYTES_MOST - 1;
	while (more > 0 && first < utf8_leads[more])
		more--;
	*code = first;
	if (more == 0 || more >= length)
		return 1;
	ULONG value = first & (UTF8_FOLLOW_MASK >> more); /* the lead's x's */
	for (ULONG i = 1; i <= more; i++)
	{
		if ((from[i] & UTF8_TAG_MASK) != UTF8_FOLLOW_TAG)
			return 1;
		value = value << UTF8_FOLLOW_BITS | (from[i] & UTF8_FOLLOW_MASK);
	}
	*code = value;
	return more + 1;
}

/**
 * bracket_skip() - Step over markup between two brackets, if it starts here.
 * @from:   the text
 * @length: its bytes
 * @open:   the character that opens the markup
 * @close:  the one that closes it
 *
 * Return: the bytes of the markup, 0 when none starts here or it never closes.
 */
static ULONG bracket_skip(const UBYTE *from, ULONG length, UBYTE open, UBYTE close)
{
	if (from[0] != open)
		return 0;
	for (ULONG i = 1; i < length; i++)
		if (from[i] == close)
			return i + 1;
	return 0;
}

/**
 * text_start() - Where a packet's text begins.
 * @from:   the packet
 * @length: its bytes
 * @codec:  DMXS_*
 *
 * Return: the offset: behind an ASS event's first fields, behind MP4 timed text's length, else 0.
 */
static ULONG text_start(const UBYTE *from, ULONG length, ULONG codec)
{
	if (codec == DMXS_MovText)
		return length >= MOVTEXT_LENGTH_BYTES ? MOVTEXT_LENGTH_BYTES : length;
	if (codec != DMXS_ASS)
		return 0;
	ULONG commas = 0, offset = 0;
	while (offset < length && commas < ASS_FIELDS_BEFORE_TEXT)
		if (from[offset++] == ',')
			commas++;
	return commas == ASS_FIELDS_BEFORE_TEXT ? offset : 0;
}

/**
 * movtext_length() - The bytes of an MP4 timed text sample that are its text, with the length before it.
 * @text:   the sample
 * @length: its bytes
 * @codec:  DMXS_*
 *
 * Return: @length for other kinds; for timed text, where the text ends and the style records begin.
 */
static ULONG movtext_length(const UBYTE *text, ULONG length, ULONG codec)
{
	if (codec != DMXS_MovText || length < MOVTEXT_LENGTH_BYTES)
		return length;
	ULONG said = (ULONG)text[0] << 8 | text[1];
	return said < length - MOVTEXT_LENGTH_BYTES ? said + MOVTEXT_LENGTH_BYTES : length;
}

/**
 * word_is() - Whether a tag's name is a word.
 * @from:   the tag's inside, between its < and >
 * @length: its bytes
 * @word:   the word, in lower case: "i", "/b"
 *
 * The name is the inside up to its first space: "i" is the name of <i>, <I>
 * and <i class="x">, not of <img>.
 *
 * Return: TRUE if it is.
 */
static BOOL word_is(const UBYTE *from, ULONG length, const char *word)
{
	ULONG i = 0;
	for (; word[i] != 0; i++)
		if (i >= length || (from[i] | ASCII_LOWER_CASE) != (UBYTE)word[i])
			return FALSE;
	return i == length || from[i] == ' ';
}

/**
 * tag_mark() - The mark an HTML-like tag of SubRip stands for.
 * @from:   the tag's inside, behind its <
 * @length: its bytes, up to its >
 *
 * Return: MARK_*, 0 for a tag that changes nothing here.
 */
static char tag_mark(const UBYTE *from, ULONG length)
{
	if (word_is(from, length, "i"))
		return MARK_ITALIC_ON;
	if (word_is(from, length, "/i"))
		return MARK_ITALIC_OFF;
	if (word_is(from, length, "b"))
		return MARK_BOLD_ON;
	if (word_is(from, length, "/b"))
		return MARK_BOLD_OFF;
	return 0;
}

/**
 * digit_between() - Whether a byte is a digit in a range.
 * @byte:  the byte
 * @first: the range's first digit
 * @last:  and its last
 *
 * Return: TRUE if it is.
 */
static BOOL digit_between(UBYTE byte, char first, char last)
{
	return byte >= (UBYTE)first && byte <= (UBYTE)last;
}

/**
 * override_top() - Whether an ASS override puts the cue at the top: an7 to an9, or the older a5 to a7.
 * @from:   behind the override's backslash
 * @length: the bytes up to the block's end
 *
 * Return: TRUE if it does.
 */
static BOOL override_top(const UBYTE *from, ULONG length)
{
	if (length < 2 || from[0] != 'a')
		return FALSE;
	return digit_between(from[1], '5', '7') || (length >= 3 && from[1] == 'n' && digit_between(from[2], '7', '9'));
}

/**
 * override_mark() - The mark one ASS override stands for.
 * @from:   behind the override's backslash
 * @length: the bytes up to the block's end
 *
 * i and b with a number switch a style on or off.
 *
 * Return: MARK_*, 0 for an override that changes nothing here.
 */
static char override_mark(const UBYTE *from, ULONG length)
{
	if (override_top(from, length))
		return MARK_TOP;
	if (length < 2 || !digit_between(from[1], '0', '9'))
		return 0;
	BOOL on = from[1] != '0';
	if (from[0] == 'i')
		return on ? MARK_ITALIC_ON : MARK_ITALIC_OFF;
	if (from[0] == 'b')
		return on ? MARK_BOLD_ON : MARK_BOLD_OFF;
	return 0;
}

/**
 * marks_put() - Write the marks a piece of markup stands for.
 * @from:   the markup, with what opens and closes it: <...> or {...}
 * @length: its bytes
 * @to:     where the marks go
 * @room:   how many fit there
 *
 * The markup leaves the plain text; what of it the renderer honours stays in
 * its place as MARK_* bytes.  A SubRip tag (<i>, </b>) stands for one mark at
 * most.  An ASS override block ({\i1\an8}) holds any number of overrides, each
 * behind a backslash, and each may stand for one: here italic on, then the cue
 * at the top.  Tags and overrides not honoured (<font>, \fs20, \pos) leave
 * nothing.
 *
 * Return: the marks written.
 */
static ULONG marks_put(const UBYTE *from, ULONG length, char *to, ULONG room)
{
	ULONG used = 0;
	/* a tag: its inside, between the < and the >, names one mark or none */
	char mark = from[0] == '<' ? tag_mark(from + 1, length - 2) : 0;
	if (mark != 0 && used < room)
		to[used++] = mark;
	/* a block: each backslash between the braces starts an override.  override_mark() gets the rest of the
	   block, from behind the backslash up to the }, and reads only the override's first letters */
	for (ULONG i = 1; from[0] == '{' && i + 1 < length; i++)
	{
		mark = from[i] == '\\' ? override_mark(from + i + 1, length - i - 2) : 0;
		if (mark != 0 && used < room)
			to[used++] = mark;
	}
	return used;
}

/**
 * markup_skip() - Step over markup that starts here: {...}, <...>.
 * @from:   the text
 * @length: its bytes
 *
 * Return: the bytes of the markup, 0 when the text here is to be kept.
 */
static ULONG markup_skip(const UBYTE *from, ULONG length)
{
	ULONG skip = bracket_skip(from, length, '{', '}');
	return skip != 0 ? skip : bracket_skip(from, length, '<', '>');
}

/**
 * line_break_here() - Whether ASS's line break, a backslash and N, starts here.
 * @from:   the text
 * @length: its bytes
 *
 * Return: TRUE if it does.
 */
static BOOL line_break_here(const UBYTE *from, ULONG length)
{
	return length >= 2 && from[0] == '\\' && (from[1] == 'N' || from[1] == 'n');
}

/**
 * text_trim() - End a plain text behind its last character that shows.
 * @plain: the text
 * @used:  its characters so far
 */
static void text_trim(char *plain, ULONG used)
{
	while (used != 0 && (plain[used - 1] == '\n' || plain[used - 1] == ' '))
		used--;
	plain[used] = 0;
}

/**
 * utf8_put() - Write a character as UTF-8.
 * @to:   where to; UTF8_BYTES_MOST bytes are free there
 * @code: the character
 *
 * Return: the bytes written.
 */
static ULONG utf8_put(char *to, ULONG code)
{
	ULONG more = 0; /* the follow bytes it takes */
	while (more + 1 < UTF8_BYTES_MOST && code >= utf8_firsts[more + 1])
		more++;
	/* the lead: its ones and zero, then the character's bits above those the follow bytes carry */
	to[0] = (char)(utf8_leads[more] | code >> (UTF8_FOLLOW_BITS * more));
	/* the follow bytes: each its 10, then the next six bits, from the highest down */
	for (ULONG i = 1; i <= more; i++)
		to[i] = (char)(UTF8_FOLLOW_TAG | (code >> (UTF8_FOLLOW_BITS * (more - i)) & UTF8_FOLLOW_MASK));
	return more + 1;
}

/**
 * text_plain() - Make a cue's text plain.
 * @plain:  gets the text: sound UTF-8, newlines between its lines, MARK_* for the markup that is honoured, cut to fit
 * @room:   its bytes
 * @from:   the text as the file holds it: UTF-8, or Latin-1, whose bytes are no sound UTF-8 and are taken one by one
 * @length: its bytes
 * @codec:  DMXS_*: what kind of markup it may hold
 */
void text_plain(char *plain, ULONG room, const char *from, ULONG length, ULONG codec)
{
	const UBYTE *text = (const UBYTE *)from;
	ULONG offset = text_start(text, length, codec), used = 0;
	length = movtext_length(text, length, codec);
	while (offset < length && text[offset] != 0 && used + UTF8_BYTES_MOST < room)
	{
		ULONG code = '\n';
		ULONG skip = markup_skip(text + offset, length - offset);
		if (skip != 0)
		{
			used += marks_put(text + offset, skip, plain + used, room - used - UTF8_BYTES_MOST);
			offset += skip;
			continue;
		}
		if (line_break_here(text + offset, length - offset))
			offset += 2;
		else
			offset += utf8_next(text + offset, length - offset, &code);
		if (code != '\r' && (code > MARK_LAST || code == '\n'))
			used += utf8_put(plain + used, code);
	}
	text_trim(plain, used);
}
