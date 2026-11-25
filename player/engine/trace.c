/* SPDX-License-Identifier: MPL-2.0 */
/*
 * trace.c - the engine's perf slots and their report (trace.h); built only
 * with KINORA_TRACE.
 */
#include <debug.h>

#include "trace.h"

#define REPORT_EVERY 2000000u /* microseconds between two reports */

static const char *const slot_names[TRACE_SLOTS] = {
    [TRACE_READ] = "read",
    [TRACE_AUDIO] = "audio",
    [TRACE_VIDEO] = "video",
    [TRACE_PASS] = "pass",
    [TRACE_ARRIVED] = "arrived_early",
    [TRACE_ARRIVED_LATE] = "arrived_late",
    [TRACE_SHOWN] = "shown_behind",
    [TRACE_DROPPED] = "dropped_behind",
    [TRACE_FLIP] = "flip",
};

static const u32 read_bounds[] = {1000, 2000, 4000, 8000, 16000, 32000, 64000, 128000}; /* microseconds */
#define READ_BOUNDS (sizeof(read_bounds) / sizeof(read_bounds[0]))

static struct perf_counter slots[TRACE_SLOTS];
static u32 read_buckets[READ_BOUNDS + 1];
static u32 reported_at;

struct perf trace_perf = {"kinora", slot_names, slots, TRACE_SLOTS};
struct perf_hist trace_reads = {"kinora", "read", read_bounds, read_buckets, READ_BOUNDS};

/**
 * trace_report() - Print the slots and the reads' spread, every REPORT_EVERY.
 *
 * Called from the engine's passes; the flipper only adds to its own slot.
 */
void trace_report(void)
{
	u32 now = get_time();
	if (now - reported_at < REPORT_EVERY)
		return;
	reported_at = now;
	perf_report(&trace_perf);
	perf_hist_report(&trace_reads);
}
