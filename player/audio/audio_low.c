/* SPDX-License-Identifier: MPL-2.0 */
/*
 * audio_low.c - the sound through AHI's low-level calls, at the file's level.
 *
 * ahi.device plays a stream at the centre, where AHI's pan law gives each
 * side half: 6 dB down, for stereo and 7.1 alike.  Here two of AHI's
 * channels, the voices its mixer adds up, play the same sample frames, each
 * frame whole with its two sides or its eight: one channel panned all the
 * way left and the other all the way right.  The law gives the left one's
 * left sides all and its right sides nothing, and the right one the other
 * way round; a 7.1 frame's centre and LFE get half from each.  Together
 * every side plays at the file's level, for twice the mixer's work.  AHI's
 * mixer then divides by its number of channels, which the master volume
 * multiplies back, as ahi.device's own "0 dB" scaling does.  Kinora's
 * volume is the master volume as well: in a mode without volume control a
 * channel's volume is only on or off.
 *
 * Both channels play LOW_BUFFERS buffers of about 20 ms round and round.
 * One AHI_Play() starts them together, and the same buffers keep them in
 * step.  AHI calls the sound hook as a channel starts a buffer, for the
 * first too, and the hook queues the next one, which AHI joins on with no
 * gap.  On the left channel's call the audio task is signalled: it tells the
 * clock the started buffer's time and fills the one that has just ended
 * from the ring.  A buffer so filled plays three buffers later, so the task
 * may be 60 ms late before a buffer plays again; AHI holds 80 ms at most.
 *
 * AHI_AllocAudioA() takes the hardware of the unit's mode (audio_unit.c).
 * A driver may refuse it while ahi.device has a unit of that hardware open
 * for another program; audio_out.c then plays through ahi.device
 * (audio_device.c).
 */
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/ahi.h>

#include "audio_private.h"

#define CHANNEL_LEFT 0u      /* the channel panned left: AHI puts even channels left in a mode that cannot pan */
#define CHANNEL_RIGHT 1u     /* the one panned right */
#define PAN_LEFT 0           /* AHIP_Pan: all to the left, which AHI's pan law plays at full volume there */
#define PAN_RIGHT 0x10000    /* all to the right */
#define MASTER_UNITY 0x10000 /* AHIET_MASTERVOLUME: the mixed sound as the mixer made it */

/**
 * sound_started() - AHI's sound hook: a channel has started a buffer; queue the next one behind it.
 * @hook:    the hook; its data is the output
 * @control: the hardware's control
 * @message: which channel
 *
 * Called inside AHI's mixing, perhaps from an interrupt: it queues and
 * signals, nothing more.  The first call of each channel is for buffer 0.
 * A buffer's sound is its number.
 *
 * Return: 0.
 */
static ULONG sound_started(struct Hook *hook asm("a0"), struct AHIAudioCtrl *control asm("a2"), struct AHISoundMessage *message asm("a1"))
{
	struct audio_out *out = hook->h_Data;
	struct low_output *low = &out->low;
	ULONG channel = message->ahism_Channel;
	ULONG started = (low->playing[channel] + 1) % LOW_BUFFERS;
	low->playing[channel] = started;
	AHI_SetSound((UWORD)channel, (UWORD)((started + 1) % LOW_BUFFERS), 0, 0, control, AHISF_NONE);
	if (channel == CHANNEL_LEFT)
	{
		low->starts++;
		Signal(low->task, 1UL << low->signal);
	}
	return 0;
}

/**
 * volume_follow() - Set the master volume for the engine's volume, if it changed.
 * @out: the output, the hardware taken
 */
static void volume_follow(struct audio_out *out)
{
	struct low_output *low = &out->low;
	ULONG volume = out->volume;
	if (volume == low->volume_set)
		return;
	low->volume_set = volume;
	struct AHIEffMasterVolume master = {
	    .ahie_Effect = AHIET_MASTERVOLUME,
	    .ahiemv_Volume = (Fixed)((unsigned long long)(ULONG)low->master_unity * volume / AUDIO_VOLUME_FULL),
	};
	AHI_SetEffect(&master, low->control);
}

/**
 * master_unity_find() - The master volume that plays each side at the file's level in the mode taken.
 * @out: the output, the hardware taken
 *
 * The mixer divides by the channels where the mode pans; where it is stereo
 * and cannot pan it divides by half as many, one here; a mono mode's divide
 * makes the two sides' mean, which is as it should be.
 */
static void master_unity_find(struct audio_out *out)
{
	struct low_output *low = &out->low;
	ULONG stereo = FALSE;
	ULONG panning = FALSE;
	AHI_GetAudioAttrs(AHI_INVALID_ID,
	                  low->control,
	                  AHIDB_Stereo, (ULONG)&stereo,
	                  AHIDB_Panning, (ULONG)&panning,
	                  TAG_DONE);
	low->master_unity = stereo && panning ? LOW_CHANNELS * MASTER_UNITY : MASTER_UNITY;
}

/**
 * hardware_free() - Stop playing and give the hardware back.
 * @out: the output; hardware not taken is left alone
 */
static void hardware_free(struct audio_out *out)
{
	struct low_output *low = &out->low;
	if (low->control == NULL)
		return;
	AHI_ControlAudio(low->control, AHIC_Play, FALSE, TAG_DONE);
	AHI_FreeAudio(low->control); /* the sounds with it; the hook is not called again */
	low->control = NULL;
}

/**
 * hardware_take() - Take the unit's hardware, two channels, and a sound for each buffer.
 * @out: the output, its buffers made
 *
 * Return: TRUE; FALSE when AHI or its driver refused, the hardware given back.
 */
static BOOL hardware_take(struct audio_out *out)
{
	struct low_output *low = &out->low;
	low->control = AHI_AllocAudio(AHIA_AudioID, unit_mode(out->setup.unit),
	                              AHIA_MixFreq, out->setup.rate,
	                              AHIA_Channels, LOW_CHANNELS,
	                              AHIA_Sounds, LOW_BUFFERS,
	                              AHIA_SoundFunc, (ULONG)&low->sound_hook,
	                              TAG_DONE);
	if (low->control == NULL)
		return FALSE;
	for (ULONG buffer = 0; buffer < LOW_BUFFERS; buffer++)
	{
		struct AHISampleInfo info = {
		    .ahisi_Type = out->sample_type,
		    .ahisi_Address = low->buffers[buffer].samples,
		    .ahisi_Length = out->chunk_frames,
		};
		if (AHI_LoadSound((UWORD)buffer, AHIST_DYNAMICSAMPLE, &info, low->control) != AHIE_OK)
		{
			hardware_free(out);
			return FALSE;
		}
	}
	master_unity_find(out);
	low->volume_set = ~0UL; /* none yet: the first follow sets it */
	return TRUE;
}

/**
 * low_start() - Fill the first buffers and start both channels on the first, together.
 * @out: the output, the hardware taken
 *
 * The last buffer is filled when the first starts, as every buffer is once
 * the one after it has.
 *
 * Return: TRUE, FALSE when AHI would not play.
 */
static BOOL low_start(struct audio_out *out)
{
	struct low_output *low = &out->low;
	for (ULONG buffer = 0; buffer + 1 < LOW_BUFFERS; buffer++)
		chunk_fill(out, &low->buffers[buffer]);
	for (ULONG channel = 0; channel < LOW_CHANNELS; channel++)
		low->playing[channel] = LOW_BUFFERS - 1; /* so the hook's first call is buffer 0's */
	low->next_start_buffer = 0;
	low->starts_handled = low->starts;
	volume_follow(out);
	AHI_Play(low->control,
	         AHIP_BeginChannel, CHANNEL_LEFT,
	         AHIP_Freq, out->setup.rate,
	         AHIP_Vol, AUDIO_VOLUME_FULL,
	         AHIP_Pan, PAN_LEFT,
	         AHIP_Sound, 0,
	         AHIP_Offset, 0,
	         AHIP_Length, 0,
	         AHIP_EndChannel, 0,
	         AHIP_BeginChannel, CHANNEL_RIGHT,
	         AHIP_Freq, out->setup.rate,
	         AHIP_Vol, AUDIO_VOLUME_FULL,
	         AHIP_Pan, PAN_RIGHT,
	         AHIP_Sound, 0,
	         AHIP_Offset, 0,
	         AHIP_Length, 0,
	         AHIP_EndChannel, 0,
	         TAG_DONE);
	return AHI_ControlAudio(low->control, AHIC_Play, TRUE, TAG_DONE) == AHIE_OK;
}

/**
 * low_restart() - Give the hardware back, take it again and start anew.
 * @out: the output
 *
 * For AHI that stopped calling the hook: the engine saw no buffer start for
 * STALL_TIME.  Hardware not taken again is tried at the next restart.
 */
static void low_restart(struct audio_out *out)
{
	out->watch.restarts++;
	out->watch.step = RESTART_CLOSING;
	hardware_free(out);
	out->watch.step = RESTART_OPENING;
	if (!hardware_take(out) || !low_start(out))
		return;
	out->watch.step = RESTART_DONE;
}

/**
 * low_follow() - Handle each start the hook has counted since the last look.
 * @out: the output
 *
 * For each, the clock is told the started buffer's time and the buffer that
 * ended is refilled.  The task may wake after more than one start: the
 * hook's count, not the buffer each channel last started, lets it handle
 * every one in turn.
 */
static void low_follow(struct audio_out *out)
{
	struct low_output *low = &out->low;
	while (low->starts_handled != low->starts)
	{
		low->starts_handled++;
		out->watch.chunks_done++;
		ULONG started = low->next_start_buffer;
		low->next_start_buffer = (started + 1) % LOW_BUFFERS;

		ObtainSemaphore(&out->lock);
		VCTime time = low->buffers[started].time; /* a flush may have taken it away */
		VCTime delay = out->delay;
		ReleaseSemaphore(&out->lock);
		if (time != VC_TIME_UNKNOWN)
			clock_follow(out->setup.clock, time + delay);
		chunk_fill(out, &low->buffers[(started + LOW_BUFFERS - 1) % LOW_BUFFERS]); /* the one that has just ended */
	}
	volume_follow(out);
}

/**
 * buffers_make() - Make the buffers, silent.
 * @out: the output
 *
 * Return: TRUE, FALSE without memory.
 */
static BOOL buffers_make(struct audio_out *out)
{
	for (ULONG buffer = 0; buffer < LOW_BUFFERS; buffer++)
		if (!chunk_make(out, &out->low.buffers[buffer]))
			return FALSE;
	return TRUE;
}

/**
 * low_open() - Open ahi.device for its library calls and take the hardware of the unit's mode.
 * @out: the output, running in the audio task
 *
 * Return: 0, or the reason: OpenDevice()'s error, ERROR_NO_FREE_STORE, ERROR_OBJECT_IN_USE when AHI refused the hardware.
 */
LONG low_open(struct audio_out *out)
{
	struct low_output *low = &out->low;
	low->signal = -1;
	low->library.ahir_Std.io_Message.mn_Length = sizeof(low->library);
	low->library.ahir_Version = AHI_VERSION;
	LONG error = OpenDevice((CONST_STRPTR)AHINAME, AHI_NO_UNIT, &low->library.ahir_Std, 0);
	if (error != 0)
		return error;
	low->library_open = TRUE;
	AHIBase = (struct Library *)low->library.ahir_Std.io_Device;

	low->signal = AllocSignal(-1);
	low->task = FindTask(NULL);
	low->sound_hook.h_Entry = (HOOKFUNC)(APTR)sound_started;
	low->sound_hook.h_Data = out;
	if (low->signal < 0 || !buffers_make(out))
		return ERROR_NO_FREE_STORE;
	return hardware_take(out) ? 0 : ERROR_OBJECT_IN_USE;
}

/**
 * low_close() - Give the hardware back, free the buffers and close ahi.device.
 * @out: the output, as far as low_open() got
 */
void low_close(struct audio_out *out)
{
	struct low_output *low = &out->low;
	hardware_free(out);
	for (ULONG buffer = 0; buffer < LOW_BUFFERS; buffer++)
		chunk_free(&low->buffers[buffer]);
	if (low->signal >= 0)
		FreeSignal(low->signal);
	low->signal = -1;
	if (low->library_open)
		CloseDevice(&low->library.ahir_Std);
	low->library_open = FALSE;
	AHIBase = NULL;
}

/**
 * low_play() - The audio task's loop: start the channels, then see to each buffer they start, until the end message comes.
 * @out:     the output, the hardware taken
 * @control: the task's own port, where the end message comes
 *
 * Return: the end message.
 */
struct Message *low_play(struct audio_out *out, struct MsgPort *control)
{
	low_start(out); /* a start that fails is seen by the stall watch, which starts anew */
	for (;;)
	{
		ULONG signals = Wait(1UL << out->low.signal | 1UL << control->mp_SigBit | RESTART_SIGNAL);
		if (signals & RESTART_SIGNAL)
			low_restart(out);
		low_follow(out);
		struct Message *message = GetMsg(control);
		if (message != NULL)
			return message;
	}
}
