/* SPDX-License-Identifier: MPL-2.0 */
/*
 * trace.h - where the engine's time goes, as emu68-common's perf slots on
 * Emu68's debug port, in a build made with KINORA_TRACE (player/CMakeLists.txt):
 * for diagnosis, never in a release.  trace_report() prints them every two
 * seconds, in the grammar emu68-common's scripts/perf-report.py reads.
 */
#ifndef KINORA_TRACE_H
#define KINORA_TRACE_H

/* What is timed: the slots of the [kinora] report */
enum trace_slot
{
	TRACE_READ,         /* one DMX_ReadPacket() */
	TRACE_AUDIO,        /* a pass's sound decoding */
	TRACE_VIDEO,        /* a pass's pictures: decoder, queue, display */
	TRACE_PASS,         /* a whole pass */
	TRACE_ARRIVED,      /* a picture from the decoder, its time still to come: how early */
	TRACE_ARRIVED_LATE, /* a picture from the decoder, its time gone: how late */
	TRACE_SHOWN,        /* a picture shown: how far behind the clock */
	TRACE_DROPPED,      /* a picture dropped: how far behind the clock */
	TRACE_FLIP,         /* the flipper's ScrollVPort() */
	TRACE_SLOTS,        /* the last: the slots before it */
};

#ifdef KINORA_TRACE
#include <debug.h>
#include <perf.h>
extern struct perf trace_perf;
extern struct perf_hist trace_reads;
#define TRACE_T0(var) PERF_T0(var)
#define TRACE_ADD(slot, var) PERF_ADD(&trace_perf, (slot), (var))
#define TRACE_VALUE(slot, us) PERF_ADD(&trace_perf, (slot), get_time() - (u32)(us)) /* a value, not a time taken */
#define TRACE_READ_ADD(var) PERF_HIST_ADD(&trace_reads, get_time() - (var))
void trace_report(void);
#else
#define TRACE_T0(var) ((void)0)
#define TRACE_ADD(slot, var) ((void)0)
#define TRACE_VALUE(slot, us) ((void)0)
#define TRACE_READ_ADD(var) ((void)0)
#define trace_report() ((void)0)
#endif

#endif /* KINORA_TRACE_H */
