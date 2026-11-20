#ifndef UNA_CLOCK_H_
#define UNA_CLOCK_H_

#include <stdbool.h>
#include <time.h>

/*
 * Local time. The RTC keeps UTC; the offset is a setting ("una/tz_min",
 * minutes east of UTC). False while the RTC has not been set.
 */
bool clock_local(struct tm *out);

#endif
