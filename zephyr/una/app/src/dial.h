#ifndef UNA_DIAL_H_
#define UNA_DIAL_H_

#include <stdbool.h>

#include <lvgl.h>

/* Analogue dials, after four German and Swiss classics. */
enum dial_style {
	DIAL_BILL,     /* Max Bill for Junghans: fine lines only */
	DIAL_RAMS,     /* Braun: numerals, batons, a yellow dot */
	DIAL_STATION,  /* the railway station clock: heavy bars */
	DIAL_TANGENTE, /* Nomos: numerals and batons in turn, a small dial at six */
	DIAL_COUNT,
};

struct dial_state {
	enum dial_style style;
	bool light; /* white dial, or black */
	int hour;
	int minute;
	int day;     /* of the month; 0 leaves the date out */
	int battery; /* percent */
	bool alarm_on;
	int alarm_hour;
	int alarm_minute;
};

const char *dial_name(enum dial_style style);

/* A dial that fills the screen. There is one; dial_set redraws it. */
lv_obj_t *dial_create(void);
void dial_set(const struct dial_state *state);

#endif
