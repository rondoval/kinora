/* SPDX-License-Identifier: MPL-2.0 */
/*
 * audio_private.h - the sound output's own structures, and what its files
 * give each other.
 *
 *   audio_out.c      the engine's calls, the ring the samples wait in, and the audio task, which picks the way to AHI
 *   audio_low.c      through AHI's low-level calls: two channels, one panned to each side, at the file's level
 *   audio_device.c   through ahi.device's writes, which share the unit: where the setup asks, or the low-level calls cannot be had
 *   audio_unit.c     the mode AHI's prefs give a unit, and what the unit takes
 */
#ifndef KINORA_AUDIO_PRIVATE_H
#define KINORA_AUDIO_PRIVATE_H

#include <exec/semaphores.h>
#include <utility/hooks.h>
#include <dos/dosextens.h>
#include <devices/ahi.h>

#include "audio_out.h"

#define MICROSECONDS_PER_SECOND 1000000
#define BLOCKS_A_SECOND 25              /* a block of ahi.device's writes is 40 ms */
#define LOW_BUFFERS_A_SECOND 50         /* a buffer of the low-level calls is 20 ms */
#define BLOCKS 2                        /* ahi.device's writes in flight at once: it starts a write linked behind a waiting one never */
#define LOW_BUFFERS 4                   /* the low-level calls' buffers, played round and round: one playing, one queued, two filled */
#define LOW_CHANNELS 2u                 /* AHI's mixer channels, not the film's: both play each whole frame, one panned left, one right */
#define MARKS 16                        /* film times the ring keeps: one for each write whose time does not follow on */
#define RESTART_SIGNAL SIGBREAKF_CTRL_E /* tells the audio task that AHI stopped playing: start it anew */
#define AHI_VERSION 4                   /* ahi.device's interface with linked requests */

/* A piece of sound handed to AHI: a buffer of the low-level calls, or what one of ahi.device's writes carries */
struct chunk
{
	APTR samples; /* chunk_frames sample frames, as the ring gives them */
	VCTime time;  /* the film's time of the first, VC_TIME_UNKNOWN for silence; audio_out_flush() clears it */
};

/* The low-level calls' state */
struct low_output
{
	/* AHI: ahi.device for its library calls, and the hardware taken through them */
	BOOL library_open;                    /* ahi.device is open without a unit */
	struct AHIRequest library;            /* that opening */
	struct AHIAudioCtrl *control;         /* AHI_AllocAudioA()'s, NULL while the hardware is not ours */

	/* what the channels play */
	struct chunk buffers[LOW_BUFFERS];    /* played in this order, round and round, on both channels at once */

	/* the sound hook, and whom it signals */
	struct Hook sound_hook;               /* AHIA_SoundFunc: AHI calls it as a channel starts a buffer */
	struct Task *task;                    /* the audio task */
	LONG signal;                          /* the signal's number, -1 for none */

	/* written by the sound hook, in AHI's mixing */
	volatile ULONG playing[LOW_CHANNELS]; /* the buffer each channel started last */
	volatile ULONG starts;                /* buffers the left channel has started */

	/* the audio task's: for each start the hook counts, it tells the clock that buffer's time and refills the one before */
	ULONG starts_handled;                 /* the starts it has done that for; behind starts while it has some to catch up on */
	ULONG next_start_buffer;              /* the buffer the next start not yet handled began: buffers start in order, round and round */

	/* the master volume */
	Fixed master_unity;                   /* the one that undoes the mixer's divide: the file's level */
	ULONG volume_set;                     /* Kinora's volume it was last set for */
};

/* One of ahi.device's writes */
struct block
{
	struct AHIRequest request; /* the write; first, so that the reply is the block */
	struct chunk chunk;        /* what it carries */
	BOOL sent;                 /* in flight */
};

/* The state of ahi.device's writes */
struct device_output
{
	struct block blocks[BLOCKS]; /* the writes in flight, sent and back in this order */
	ULONG newest;                /* the block sent last: the next one is linked behind it */
	struct MsgPort *port;        /* where the writes come back */
	BOOL unit_open;              /* ahi.device is open on the unit */
};

/* The message that starts and ends the task */
struct task_message
{
	struct Message message; /* Exec's part */
	struct audio_out *out;  /* the output the task serves */
	LONG error;             /* the start's answer: 0, or ahi.device's error */
};

/* How far the last restart got (audio_out_counts() reports the number) */
enum restart_step : ULONG
{
	RESTART_NONE = 0,     /* none yet */
	RESTART_ASKED = 1,    /* the engine signalled the audio task */
	RESTART_CLOSING = 2,  /* the task is letting go of AHI: taking the blocks back and closing the unit, or freeing the hardware */
	RESTART_OPENING = 3,  /* it has let go; it is opening the unit or taking the hardware again */
	RESTART_DONE = 4,     /* AHI plays again */
};

/* The film time of one sample frame written to the ring */
struct mark
{
	ULONG position; /* which frame: its number among those written since the last flush, the first being 0 */
	VCTime time;    /* its film time */
};

/* Decoded sample frames between the engine and the audio task, in a buffer used round and round */
struct ring
{
	UBYTE *samples;           /* the buffer: capacity sample frames of frame_bytes each */
	ULONG capacity;           /* its size in sample frames */
	ULONG head;               /* index in samples of the next sample frame to play */
	ULONG count;              /* sample frames written and not yet played, from head on, past the end back to the start */
	ULONG head_position;      /* the position of the frame at head: the sample frames moved into chunks since the last flush */
	struct mark marks[MARKS]; /* oldest first; a frame's time follows on from the last mark at or before it */
	ULONG mark_count;         /* marks in use; 0: the frames have no time */
};

/* The stall watch: the engine sees the count stand still, the audio task starts AHI anew */
struct watch
{
	volatile ULONG chunks_done;      /* audio task: blocks back from AHI, or buffers it started */
	volatile ULONG restarts;         /* audio task: times AHI was started anew */
	volatile enum restart_step step; /* the last restart's; RESTART_ASKED is the engine's, the rest the task's */
	ULONG seen_done;                 /* engine: chunks_done as it last saw it */
	VCTime seen_at;                  /* engine: clock_wall() when it first saw that count */
};

/* The output */
struct audio_out
{
	/* fixed by audio_out_open() */
	struct audio_setup setup;     /* what the engine asked for; its delay is copied to delay */
	ULONG sample_type;            /* ahir_Type: AHIST_S16S, AHIST_S32S or AHIST_L7_1 */
	ULONG frame_bytes;            /* bytes of a sample frame */
	ULONG chunk_frames;           /* sample frames of a chunk, by the way to AHI the task took */

	/* shared by the engine and the audio task */
	struct SignalSemaphore lock;  /* guards ring, running, delay and the chunks' times */
	struct ring ring;             /* what is waiting to be played */
	BOOL running;                 /* the ring is played; else silence */
	volatile ULONG volume;        /* 0 to AUDIO_VOLUME_FULL; one long the engine writes, so it needs no lock */
	VCTime delay;                 /* microseconds the sound comes after the picture; negative for before */

	/* the audio task's: one of the two ways to AHI */
	BOOL low_level;               /* the low-level calls play; else ahi.device's writes */
	struct low_output low;        /* the low-level calls' */
	struct device_output device;  /* ahi.device's writes' */

	/* the engine's: the audio task's start and end */
	struct Process *task;         /* the audio task, NULL when it did not come up */
	struct MsgPort *control;      /* its own port, where the end message goes: its pr_MsgPort is dos.library's */
	struct MsgPort *reply_port;   /* where its answers come back */
	struct task_message message;  /* the start and end message */

	struct watch watch;           /* AHI's stall watch */
};

/* audio_out.c */
BOOL chunk_make(struct audio_out *out, struct chunk *chunk);
void chunk_free(struct chunk *chunk);
void chunk_fill(struct audio_out *out, struct chunk *chunk);

/* audio_low.c */
LONG low_open(struct audio_out *out);
void low_close(struct audio_out *out);
struct Message *low_play(struct audio_out *out, struct MsgPort *control);

/* audio_device.c */
LONG device_open(struct audio_out *out);
void device_close(struct audio_out *out);
struct Message *device_play(struct audio_out *out, struct MsgPort *control);

/* audio_unit.c */
ULONG unit_mode(ULONG unit);

#endif /* KINORA_AUDIO_PRIVATE_H */
