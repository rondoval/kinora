/* SPDX-License-Identifier: MPL-2.0 */
/*
 * clock.c - the player's clock (clock.h has the model).
 */
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>

#include "clock.h"

#define MICROSECONDS_PER_SECOND 1000000ULL
#define FOLLOW_SHARE 16     /* a correction moves the clock 1/FOLLOW_SHARE of the way to the audio's time */
#define FOLLOW_SNAP 100000  /* microseconds: a clock off by more jumps to the audio's time (after a stall or a seek) */

struct Device *TimerBase;

static struct timerequest timer_request; /* timer.device, open for ReadEClock() */
static ULONG eclock_rate;                /* E-clock ticks per second */

/**
 * clock_open() - Open timer.device for ReadEClock().
 *
 * Return: TRUE, or FALSE when timer.device does not open.
 */
BOOL clock_open(void)
{
	if (OpenDevice((CONST_STRPTR) "timer.device", UNIT_MICROHZ, &timer_request.tr_node, 0) != 0)
		return FALSE;
	TimerBase = timer_request.tr_node.io_Device;
	struct EClockVal value;
	eclock_rate = ReadEClock(&value);
	return TRUE;
}

/**
 * clock_close() - Close timer.device, if clock_open() opened it.
 */
void clock_close(void)
{
	if (TimerBase != NULL)
		CloseDevice(&timer_request.tr_node);
	TimerBase = NULL;
}

/**
 * clock_wall() - Read the E-clock in microseconds.
 *
 * Return: microseconds since the machine started.
 */
VCTime clock_wall(void)
{
	struct EClockVal value;
	ReadEClock(&value);
	unsigned long long ticks = (unsigned long long)value.ev_hi << 32 | value.ev_lo;
	return (VCTime)(ticks / eclock_rate * MICROSECONDS_PER_SECOND + ticks % eclock_rate * MICROSECONDS_PER_SECOND / eclock_rate);
}

/**
 * clock_set() - Stop the clock at a film time.
 * @clock: the clock
 * @media: the film time, microseconds
 *
 * A waiting clock stops waiting.
 */
void clock_set(struct clock *clock, VCTime media)
{
	Forbid();
	clock->media = media;
	clock->running = FALSE;
	clock->waiting = FALSE;
	Permit();
}

/**
 * clock_place() - Move a clock that is not running to a film time.
 * @clock: the clock
 * @media: the film time, microseconds
 *
 * A stopped clock stays stopped and a waiting one keeps waiting.  A running
 * clock is left alone: the audio task started it at the audio's time.
 */
void clock_place(struct clock *clock, VCTime media)
{
	Forbid();
	if (!clock->running)
		clock->media = media;
	Permit();
}

/**
 * clock_run() - Start a clock that is not running from its film time.
 * @clock:          the clock
 * @wait_for_audio: TRUE: wait instead; the audio task's next clock_follow() starts it at the time it names
 *
 * A running clock is left alone.
 */
void clock_run(struct clock *clock, BOOL wait_for_audio)
{
	VCTime wall = clock_wall();
	Forbid();
	if (!clock->running)
	{
		clock->wall = wall;
		clock->running = !wait_for_audio;
		clock->waiting = wait_for_audio;
	}
	Permit();
}

/**
 * clock_stop() - Stop the clock at the film time now.
 * @clock: the clock
 */
void clock_stop(struct clock *clock)
{
	clock_set(clock, clock_now(clock));
}

/**
 * clock_now() - Read the film time now.
 * @clock: the clock
 *
 * Return: the film time, microseconds.
 */
VCTime clock_now(const struct clock *clock)
{
	VCTime wall = clock_wall();
	Forbid();
	VCTime media = clock->running ? clock->media + (wall - clock->wall) : clock->media;
	Permit();
	return media;
}

/**
 * clock_follow() - Correct the clock to the audio: the block AHI starts playing now is at this film time.
 * @clock: the clock
 * @media: the block's film time, microseconds
 *
 * Context: the audio task.
 *
 * A waiting clock starts at @media.  A running one moves 1/FOLLOW_SHARE of
 * the way there, or all the way when it is more than FOLLOW_SNAP off.  A
 * stopped one stays.
 */
void clock_follow(struct clock *clock, VCTime media)
{
	VCTime wall = clock_wall();
	Forbid();
	if (clock->waiting)
	{
		clock->media = media;
		clock->wall = wall;
		clock->running = TRUE;
		clock->waiting = FALSE;
	}
	else if (clock->running)
	{
		VCTime now = clock->media + (wall - clock->wall);
		VCTime off = media - now;
		BOOL far = off > FOLLOW_SNAP || off < -FOLLOW_SNAP;
		clock->media = far ? media : now + off / FOLLOW_SHARE;
		clock->wall = wall;
	}
	Permit();
}
