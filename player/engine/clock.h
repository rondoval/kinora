/* SPDX-License-Identifier: MPL-2.0 */
/*
 * clock.h - the player's clock: the film time now.
 *
 * The film time advances with timer.device's E-clock.  With an audio
 * stream, the audio task corrects it each time AHI starts playing a block
 * (clock_follow()), by only part of the difference: a single block's start
 * time strays by 10 to 20 ms, while the average is exact.  Without audio
 * the E-clock alone drives it.
 *
 *   stopped   the film time stays where clock_set() or clock_stop() left it
 *   waiting   stopped; the audio task's next clock_follow() starts it at the time it names
 *   running   the film time advances with the E-clock
 */
#ifndef KINORA_ENGINE_CLOCK_H
#define KINORA_ENGINE_CLOCK_H

#include <exec/types.h>
#include <libraries/videocodec.h>

/*
 * The engine task sets, starts, stops and reads the clock; the audio task
 * corrects it (clock_follow()) at a higher priority, so it can preempt the
 * engine in the middle of an update.  The fields change together, and
 * media and wall take two moves each, so every function that touches them
 * does so under Forbid().
 */
struct clock
{
	VCTime media; /* film time, microseconds: where it stands while stopped, where it stood at wall while running */
	VCTime wall;  /* clock_wall() at the last start or correction; read only while running */
	BOOL running; /* TRUE: the film time is media + (clock_wall() - wall) */
	BOOL waiting; /* TRUE: stopped until the audio task's next clock_follow() starts it */
};

BOOL clock_open(void);
void clock_close(void);
VCTime clock_wall(void);

void clock_set(struct clock *clock, VCTime media);
void clock_place(struct clock *clock, VCTime media);
void clock_run(struct clock *clock, BOOL wait_for_audio);
void clock_stop(struct clock *clock);
VCTime clock_now(const struct clock *clock);
void clock_follow(struct clock *clock, VCTime media);

#endif /* KINORA_ENGINE_CLOCK_H */
