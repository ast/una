#ifndef UNA_FIT_H_
#define UNA_FIT_H_

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/*
 * Writes one activity file in Garmin's FIT format, a message at a time, so
 * that what has been recorded is on the disk while it is being recorded.
 * One file at a time; all from one thread.
 */

/* The messages, each with its values in this order. */
enum fit_message {
	FIT_FILE_ID,  /* time created */
	FIT_EVENT,    /* timestamp, event type (FIT_EVENT_*) */
	FIT_RECORD,   /* timestamp, latitude, longitude, distance cm, speed mm/s, altitude */
	FIT_LAP,      /* written by fit_lap */
	FIT_SESSION,  /* written by fit_end */
	FIT_ACTIVITY, /* written by fit_end */
	FIT_MESSAGE_COUNT,
};

#define FIT_EVENT_START 0
#define FIT_EVENT_STOP  4 /* "stop all" */

/* Values that say "none". */
#define FIT_NONE        0xffffffffu
#define FIT_NO_POSITION 0x7fffffffu

enum fit_sport {
	FIT_RUN = 1,
	FIT_RIDE = 2,
	FIT_WALK = 11,
	FIT_HIKE = 17,
};

/* What the file holds so far, kept as it is written or found by fit_resume. */
struct fit_summary {
	uint32_t start;       /* FIT time: the second before the first record */
	uint32_t end;         /* of the last */
	uint32_t records;     /* one a second while the timer runs */
	uint32_t distance_cm;
	uint32_t laps;
	/* Where the lap being recorded began. */
	uint32_t lap_start;
	uint32_t lap_records;
	uint32_t lap_distance_cm;
};

/* Seconds since the last day of 1989, UTC. */
uint32_t fit_time(time_t unix_time);
time_t fit_unix_time(uint32_t fit_time);

/* Positions are in "semicircles": 2^31 of them are 180 degrees. */
int32_t fit_semicircles(int64_t nanodegrees);
/* Altitude in metres as a record has it. */
uint32_t fit_altitude(int32_t millimetres);

int fit_begin(const char *path, uint32_t created);
/* Take up a file that was never ended: what is whole of it is kept. */
int fit_resume(const char *path);
bool fit_is_open(void);
const struct fit_summary *fit_summary(void);

int fit_put(enum fit_message message, const uint32_t *values);
/* Close the lap being recorded. */
int fit_lap(bool session_end);
int fit_sync(void);
/* Close the last lap, the session and the file. `ascent_m` may be FIT_NONE. */
int fit_end(enum fit_sport sport, uint32_t ascent_m, int32_t utc_offset_s);

/* Change the sport of a file that fit_end has written. */
int fit_set_sport(const char *path, enum fit_sport sport);

#endif
