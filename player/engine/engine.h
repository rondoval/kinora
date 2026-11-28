/* SPDX-License-Identifier: MPL-2.0 */
/*
 * engine.h - the player's engine, as its front sees it.
 *
 * The engine is a process of its own: it reads the file, feeds both decoders,
 * hands sound to the audio output and shows each picture when the clock
 * reaches it (docs/architecture.md section 6).  The front sends it commands; each
 * call here is one message, answered when the engine has done it.  When
 * something the front should look at happens (the file ended, playing
 * failed) the engine signals the front, which asks with engine_status().
 *
 *   engine = engine_start(&setup);
 *   engine_open(engine, name, &place);
 *   engine_play(engine, TRUE);
 *   ...
 *   engine_end(engine);
 */
#ifndef KINORA_ENGINE_H
#define KINORA_ENGINE_H

#include <exec/types.h>
#include <exec/tasks.h>
#include <libraries/videocodec.h>

#include "../display/display.h"

struct engine; /* the engine; its own */

/* What the engine is doing (engine_status.state) */
enum engine_state
{
	ENGINE_EMPTY,   /* no file */
	ENGINE_PAUSED,  /* a file, standing still */
	ENGINE_PLAYING, /* a file, running */
	ENGINE_ENDED,   /* a file, played to its end */
};

/* Why something failed (engine_open(), engine_status.error): negative, 0 for nothing */
#define ENGINE_ERR_NOMEM (-1)   /* out of memory */
#define ENGINE_ERR_FILE (-2)    /* the file did not open or is no container demuxer.library reads */
#define ENGINE_ERR_NOSTREAM (-3) /* nothing in the file that can be played here */
#define ENGINE_ERR_VIDEO (-4)   /* the video decoder refused or failed */
#define ENGINE_ERR_SEEK (-5)    /* the file cannot be moved in */

/* What engine_start() needs to know */
struct engine_setup
{
	struct Task *listener; /* the front's task */
	ULONG signal;          /* the bits it is signalled with */
	ULONG audio_unit;      /* the ahi.device unit */
	BOOL audio_shared;     /* share the unit with other programs, 6 dB low; else take its hardware, at the file's level */
};

/* The engine's answer to engine_status() */
struct engine_status
{
	enum engine_state state; /* what it is doing */
	LONG error;              /* ENGINE_ERR_* of what stopped the playing, 0 for nothing */
	VCTime position;         /* the film's time now */
	VCTime duration;         /* its length, VC_TIME_UNKNOWN when the file does not say */
	BOOL seekable;           /* engine_seek() can work */
	BOOL has_video;          /* a video stream is decoded */
	BOOL has_audio;          /* an audio stream is decoded */
	ULONG width;             /* the picture's pixels as the stream has them */
	ULONG height;            /* and rows */
	ULONG frames_shown;      /* pictures shown since the file was opened */
	ULONG frames_dropped;    /* pictures that came too late and were not shown */
	VCTime audio_delay;      /* how far the sound is put behind the picture */
	VCTime subtitle_delay;   /* how far the subtitles are */
};

#define STATISTICS_TEXT_SIZE 1024 /* the statistics as text, with its NUL */
#define ENGINE_TRACKS 24          /* the tracks engine_tracks() lists at most */
#define ENGINE_TRACK_OFF (-1L)    /* engine_select(): no track of the kind */
#define ENGINE_TRACK_FILE (-2L)   /* engine_select(), subtitles: the SubRip file given with engine_subtitle_file() */

/* What a track is (engine_track.kind, engine_select()) */
enum engine_track_kind
{
	ENGINE_TRACK_AUDIO,
	ENGINE_TRACK_SUBTITLE,
};

/* One audio or subtitle track of the file (engine_tracks()) */
struct engine_track
{
	LONG stream;      /* its number in the file, for engine_select(); ENGINE_TRACK_FILE for the subtitle file */
	enum engine_track_kind kind; /* audio or subtitle */
	ULONG channels;   /* an audio track's channels */
	BOOL selected;    /* it is the one played or shown */
	char language[4]; /* empty when the file does not say */
	char title[64];   /* empty when the file does not say */
};

#define ENGINE_CHAPTERS 64 /* the chapters engine_chapters() lists at most */

/* One chapter of the film (engine_chapters()) */
struct engine_chapter
{
	VCTime start;   /* where it begins */
	char title[64]; /* empty when the file does not say */
};

struct engine *engine_start(const struct engine_setup *setup);
ULONG engine_chapters(struct engine *engine, struct engine_chapter *chapters, ULONG room);
void engine_delays(struct engine *engine, VCTime audio, VCTime subtitles);
void engine_aspect(struct engine *engine, LONG width, LONG height);
ULONG engine_tracks(struct engine *engine, struct engine_track *tracks, ULONG room);
LONG engine_select(struct engine *engine, enum engine_track_kind kind, LONG stream);
void engine_audio_unit(struct engine *engine, ULONG unit, BOOL shared);
LONG engine_subtitle_file(struct engine *engine, CONST_STRPTR name);
LONG engine_subtitle_font(struct engine *engine, CONST_STRPTR name);
void engine_subtitle_look(struct engine *engine, ULONG size, ULONG distance);
void engine_statistics(struct engine *engine, BOOL panel, char *text, ULONG room);
void engine_end(struct engine *engine);
LONG engine_open(struct engine *engine, CONST_STRPTR name, const struct display_place *place);
void engine_close(struct engine *engine);
void engine_play(struct engine *engine, BOOL play);
LONG engine_seek(struct engine *engine, VCTime time);
void engine_volume(struct engine *engine, ULONG volume);
LONG engine_place(struct engine *engine, const struct display_place *place);
void engine_status(struct engine *engine, struct engine_status *status);

#endif /* KINORA_ENGINE_H */
