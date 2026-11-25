/* SPDX-License-Identifier: MPL-2.0 */
/*
 * audio_unit.c - what a unit of ahi.device plays: the mode AHI's prefs give
 * it, and the samples that mode takes.
 *
 * AHI's low-level calls know no units: they ask for the hardware by the
 * mode.  No AHI call says which mode a unit plays in; only AHI's prefs file
 * does, and it is read here as ahi.device reads it.  The mode also says
 * whether the unit plays 7.1, so the engine knows how many channels to ask
 * the decoder for.
 */
#include <dos/dos.h>
#include <libraries/iffparse.h>
#include <prefs/prefhdr.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/iffparse.h>
#include <proto/ahi.h>

#include "audio_private.h"

#define AHI_PREFS "ENV:Sys/ahi.prefs" /* where AHI's prefs give each unit its mode */
#define AHI_WIDE_VERSION 6            /* ahi.device's first with 32-bit sample types (AHIST_S32S, AHIST_L7_1) and 7.1 modes */

/*
 * ahi.device's base, for its library calls, while an opening without a unit
 * stands: the engine's question about a unit, or the audio task's low-level
 * calls.  The two never overlap: the engine asks only while no output is open.
 */
struct Library *AHIBase;

/**
 * collected_mode() - The mode of a unit among the AHIU chunks read.
 * @iff:  the prefs file, parsed
 * @unit: the unit
 * @mode: what to give when no chunk is the unit's
 *
 * Return: the unit's mode's ID, or @mode.
 */
static ULONG collected_mode(struct IFFHandle *iff, ULONG unit, ULONG mode)
{
	for (struct CollectionItem *item = FindCollection(iff, ID_PREF, ID_AHIU); item != NULL; item = item->ci_Next)
	{
		const struct AHIUnitPrefs *prefs = item->ci_Data;
		if ((ULONG)item->ci_Size >= sizeof(*prefs) && prefs->ahiup_Unit == unit)
			mode = prefs->ahiup_AudioMode;
	}
	return mode;
}

/**
 * unit_mode() - The audio mode AHI's prefs give a unit of ahi.device.
 * @unit: the unit
 *
 * Return: the mode's ID, AHI_DEFAULT_ID when the prefs name none.
 */
ULONG unit_mode(ULONG unit)
{
	ULONG mode = AHI_DEFAULT_ID;
	struct IFFHandle *iff = AllocIFF();
	if (iff == NULL)
		return mode;
	iff->iff_Stream = (ULONG)Open((CONST_STRPTR)AHI_PREFS, MODE_OLDFILE);
	if (iff->iff_Stream != 0)
	{
		InitIFFasDOS(iff);
		if (OpenIFF(iff, IFFF_READ) == 0)
		{
			if (CollectionChunk(iff, ID_PREF, ID_AHIU) == 0 && StopOnExit(iff, ID_PREF, ID_FORM) == 0)
				ParseIFF(iff, IFFPARSE_SCAN);
			mode = collected_mode(iff, unit, mode);
			CloseIFF(iff);
		}
		Close((BPTR)iff->iff_Stream);
	}
	FreeIFF(iff);
	return mode;
}

/**
 * abilities_read() - Ask the open ahi.device what a unit takes.
 * @device:    ahi.device, opened without a unit
 * @unit:      the unit
 * @abilities: filled in
 */
static void abilities_read(struct Device *device, ULONG unit, struct audio_abilities *abilities)
{
	abilities->wide = device->dd_Library.lib_Version >= AHI_WIDE_VERSION;
	if (!abilities->wide)
		return; /* 7.1 came with the 32-bit samples, its only ones */
	ULONG multichannel = FALSE;
	AHIBase = &device->dd_Library;
	AHI_GetAudioAttrs(unit_mode(unit), NULL, AHIDB_MultiChannel, (ULONG)&multichannel, TAG_DONE);
	AHIBase = NULL;
	abilities->surround = multichannel != FALSE;
}

/**
 * audio_out_abilities() - What a unit of ahi.device takes: 32-bit samples, and 7.1.
 * @unit:      the unit
 * @abilities: filled in; all FALSE without ahi.device
 *
 * Opens the device without a unit for its version and its calls.  Called
 * with no output open.
 */
void audio_out_abilities(ULONG unit, struct audio_abilities *abilities)
{
	*abilities = (struct audio_abilities){0};
	struct MsgPort *port = CreateMsgPort();
	if (port == NULL)
		return;
	struct AHIRequest *request = (struct AHIRequest *)CreateIORequest(port, sizeof(*request));
	if (request != NULL)
	{
		request->ahir_Version = AHI_VERSION;
		if (OpenDevice((CONST_STRPTR)AHINAME, AHI_NO_UNIT, &request->ahir_Std, 0) == 0)
		{
			abilities_read(request->ahir_Std.io_Device, unit, abilities);
			CloseDevice(&request->ahir_Std);
		}
		DeleteIORequest(&request->ahir_Std);
	}
	DeleteMsgPort(port);
}
