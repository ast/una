#ifndef UNA_MOTION_H_
#define UNA_MOTION_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * How the watch is moved: the steps of the day, and the turn of the wrist
 * for a look at the dial, which lights it. Both are the accelerometer's
 * own doing; it is asked for its count once a minute.
 */

/* As the sensor has them. */
enum motion_activity {
	MOTION_STILL,
	MOTION_WALKING,
	MOTION_RUNNING,
	MOTION_UNKNOWN,
};

struct motion_status {
	bool present;         /* there is a sensor and it started */
	uint32_t steps_today; /* since midnight, or since boot on the day of it */
	uint32_t steps;       /* since boot */
	enum motion_activity activity;
	uint32_t wrist_turns; /* since boot */
	uint32_t failures;    /* readings that failed, since boot */
};

/* What the last reading left, without touching the bus. */
void motion_status(struct motion_status *status);

/* Have it read now; returns at once. */
void motion_poll(void);

/* Whether a turn of the wrist lights the screen. Stored. */
bool motion_wrist_light(void);
void motion_set_wrist_light(bool on);

/*
 * The sensor itself: src/motion_hw.c, or the simulator's stand-in.
 * Acceleration in thousandths of g, x to three o'clock, y to twelve, z out
 * of the dial: lying on its back, the watch reads 0 0 1000.
 */
int motion_hw_start(void);
int motion_hw_read(uint32_t *steps, enum motion_activity *activity, int32_t accel_mg[3]);
/* The acceleration alone, for whoever needs to know which way is down. */
int motion_hw_accel(int32_t accel_mg[3]);
int motion_hw_wrist(bool on);
/* Called by it, from any thread, when the wrist is turned. */
void motion_wrist_turned(void);

#endif
