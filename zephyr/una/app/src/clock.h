#ifndef UNA_CLOCK_H_
#define UNA_CLOCK_H_

#include <stdbool.h>
#include <time.h>

/*
 * Local time. The RTC keeps UTC; the offset is a setting ("una/tz_min",
 * minutes east of UTC). False while the RTC has not been set.
 */
bool clock_local(struct tm *out);

/* Seconds since 1970, UTC; 0 while the RTC has not been set. */
time_t clock_utc(void);

/* Seconds east of UTC. */
int clock_utc_offset(void);

/* Set the RTC, and the offset in minutes east of UTC, which is stored. */
int clock_set(time_t utc, int offset_min);

#endif
