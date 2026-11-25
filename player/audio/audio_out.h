/* SPDX-License-Identifier: MPL-2.0 */
/*
 * audio_out.h - the player's sound: decoded samples to AHI, and the clock kept to them.
 *
 * A task of its own, above the range AmiKit's Executive re-prioritises,
 * owns AHI.  It plays chunks of 20 or 40 ms, filled from a ring the engine
 * writes into, and at every chunk AHI starts on it tells the clock which time
 * of the film that chunk is.  The sound goes through AHI's low-level calls,
 * which take the unit's hardware and play at the file's level; or, when the
 * setup shares the unit or AHI's driver refuses those calls, through
 * ahi.device's writes, which share it with other programs and play 6 dB low
 * (audio_out_low_level() says which).
 *
 * The engine's side:
 *
 *   out = audio_out_open(&setup, &error);
 *   audio_out_run(out, TRUE);
 *   while (...)
 *       if (audio_out_room(out) >= frames)             // else wait for setup.signal
 *           audio_out_write(out, samples, frames, time);
 *   audio_out_close(out);
 *
 * Samples are what the AHI sample type takes: two LONGs a sample frame for
 * stereo (two WORDs where ahi.device is older than version 6), eight LONGs
 * in AHI's order for 7.1 (audio_out_place() makes those from
 * audiocodec.library's); audio_out_abilities() says which a unit takes.
 */
#ifndef KINORA_AUDIO_OUT_H
#define KINORA_AUDIO_OUT_H

#include <exec/types.h>
#include <exec/tasks.h>
#include <libraries/videocodec.h>

#include "../engine/clock.h"

#define AUDIO_STEREO_CHANNELS 2   /* samples in a sample frame of a stereo mode */
#define AUDIO_SURROUND_CHANNELS 8 /* and of a 7.1 mode */
#define AUDIO_VOLUME_FULL 0x10000 /* audio_out_volume(): the file's level, as far as AHI plays it */

struct audio_out; /* the output; audio_out.c's own */

/* Where AHI's 7.1 channels are in a decoder's sample frame (audio_out_placement()) */
struct audio_placement
{
	ULONG layout;                         /* ACCH_*: the decoder's channels it was worked out for */
	ULONG channels;                       /* how many those are: the samples in a decoder's sample frame */
	BYTE source[AUDIO_SURROUND_CHANNELS]; /* for each of AHI's channels, its sample in the decoder's frame; -1: silent */
};

/* What a unit of ahi.device takes (audio_out_abilities()) */
struct audio_abilities
{
	BOOL wide;     /* 32-bit samples: ahi.device is version 6 or later; else 16-bit */
	BOOL surround; /* 7.1: the unit's mode is a 7.1 one, and the samples are 32-bit, 7.1's only type */
};

/* What audio_out_open() needs to know */
struct audio_setup
{
	ULONG unit;           /* the ahi.device unit */
	BOOL shared;          /* share the unit with other programs through ahi.device's writes; else take its hardware */
	ULONG rate;           /* sample frames a second */
	ULONG channels;       /* AUDIO_STEREO_CHANNELS or AUDIO_SURROUND_CHANNELS */
	BOOL wide;            /* 32-bit samples, else 16-bit; TRUE for 7.1, whose only sample type is 32-bit */
	VCTime delay;         /* the sound is put behind the picture by this much: the clock runs ahead of the chunk AHI starts on */
	struct clock *clock;  /* the clock to keep to the sound */
	struct Task *listener; /* the task to tell that the ring has room again */
	ULONG signal;         /* with these signal bits */
};

void audio_out_abilities(ULONG unit, struct audio_abilities *abilities);
struct audio_out *audio_out_open(const struct audio_setup *setup, LONG *error);
void audio_out_close(struct audio_out *out);
ULONG audio_out_room(struct audio_out *out);
ULONG audio_out_queued(struct audio_out *out);
ULONG audio_out_write(struct audio_out *out, CONST_APTR samples, ULONG frames, VCTime time);
void audio_out_run(struct audio_out *out, BOOL run);
void audio_out_watch(struct audio_out *out);
void audio_out_counts(const struct audio_out *out, ULONG *chunks, ULONG *restarts, ULONG *step);
BOOL audio_out_low_level(const struct audio_out *out);
void audio_out_flush(struct audio_out *out);
void audio_out_volume(struct audio_out *out, ULONG volume);
void audio_out_delay(struct audio_out *out, VCTime delay);
void audio_out_placement(ULONG layout, struct audio_placement *placement);
void audio_out_place(const LONG *samples, ULONG frames, const struct audio_placement *placement, LONG *placed);

#endif /* KINORA_AUDIO_OUT_H */
