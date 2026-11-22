#ifndef UNA_COMPASS_H_
#define UNA_COMPASS_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * The compass: the magnetometer, on while anyone wants it, and the
 * accelerometer for which way is down.
 */

/* Who may want it on. */
enum compass_user {
	COMPASS_FOR_PAGE,
	COMPASS_FOR_SHELL,
};

/* Returns at once; the first heading is there within a second. */
void compass_want(enum compass_user user, bool on);

struct compass_reading {
	/*
	 * Where twelve o'clock points, in degrees clockwise from magnetic
	 * north, however the watch is held. Good to a few degrees once
	 * calibrated; before that it may be anything.
	 */
	int heading_deg;
	int tilt_deg;      /* of the dial from lying level */
	int32_t field_nt;  /* the field's strength as corrected: 25 to 65 uT on earth */
	bool level_assumed; /* no accelerometer: right only with the dial level */
};

/* False when there is none, or none fresh enough to use. */
bool compass_read(struct compass_reading *reading);

/*
 * The watch has a field of its own, which is found by turning it every way
 * round: what stays the same is the watch's. Calibration begins with this
 * and ends by itself when the watch has been turned enough, or with
 * compass_calibrate(false), which keeps what was found only if it was
 * enough. Stored.
 */
void compass_calibrate(bool on);

struct compass_status {
	bool present; /* there is a sensor; known once it has been on */
	bool on;
	bool calibrated;
	bool calibrating;
	int progress;         /* of a calibration, percent */
	uint32_t readings;    /* since it was turned on */
	uint32_t failures;    /* since boot */
	int32_t raw_nt[3];    /* the field as the sensor has it */
	int32_t offset_nt[3]; /* the watch's own, taken off it */
};

void compass_status(struct compass_status *status);

/*
 * The sensor itself: src/compass_hw.c, or the simulator's stand-in. The
 * field in nT, x to three o'clock, y to twelve, z out of the dial.
 */
int compass_hw_power(bool on);
int compass_hw_sample(int32_t field_nt[3]);

#endif
