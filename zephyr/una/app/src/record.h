#ifndef UNA_RECORD_H_
#define UNA_RECORD_H_

#include <stdbool.h>
#include <stdint.h>

#include "fit.h"

/*
 * The recorder: one for every activity. It keeps time, follows the GNSS
 * fixes and writes a FIT file as it goes; what the activity was is a label
 * given when it is over.
 */

enum record_phase {
	RECORD_IDLE,
	RECORD_RUNNING,
	RECORD_PAUSED,
};

struct record_state {
	enum record_phase phase;
	bool gnss;           /* there is a receiver */
	bool fix;            /* and it knows where it is */
	bool saved;          /* the file is being, or was, written */
	uint32_t timer_s;    /* not counting pauses */
	uint32_t distance_m;
	uint32_t speed_mm_s; /* now; 0 when not known */
	uint32_t ascent_m;
	enum fit_sport sport; /* once finished: a guess, or what record_label set */
};

/*
 * While one is recorded, of that one; otherwise of the last, with timer_s 0
 * when there has been none since the watch started.
 */
void record_get(struct record_state *state);
bool record_active(void);

/* From the UI. They return at once; the work is done by the recorder's thread. */
void record_start(void);
void record_pause(void);
void record_resume(void);
void record_finish(void);
/* Say what the last one was. */
void record_label(enum fit_sport sport);

#endif
