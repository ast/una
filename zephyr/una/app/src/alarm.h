#ifndef UNA_ALARM_H_
#define UNA_ALARM_H_

#include <stdbool.h>

/* The daily alarm's time; false when it is off. */
bool alarm_get(int *hour, int *minute);

/* The clock was set: work out again when the alarm is due. */
void alarm_clock_changed(void);

#endif
