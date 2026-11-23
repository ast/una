#ifndef UNA_HR_H_
#define UNA_HR_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * The pulse: the sensor on the back, on while anyone wants it, looks for
 * skin and takes the green light that comes back through it twenty times a
 * second, in lots of a second. src/hr_alg.c makes the pulse of that and of
 * how the wrist is moved.
 */

/* Who may want it on. */
enum hr_user {
	HR_FOR_RECORD,
	HR_FOR_PAGE,
	HR_FOR_PHONE,
	HR_FOR_SHELL,
};

/* Returns at once; a pulse is there some ten seconds after skin is found. */
void hr_want(enum hr_user user, bool on);
bool hr_wanted(enum hr_user user);

struct hr_status {
	bool present; /* there is a sensor; known once it has been on */
	bool on;
	bool worn;
	uint8_t bpm;     /* 0 while there is none to give */
	uint8_t quality; /* of the last second, 0 to 100 */
	bool moving;
	uint32_t batches;  /* since it was turned on */
	uint32_t failures; /* since boot */
};

void hr_status(struct hr_status *status);

/* The pulse, beats a minute; false when there is none now. */
bool hr_read(uint8_t *bpm);

/*
 * The sensor itself: src/hr_hw.c, or the simulator's stand-in. The light of
 * each channel is scaled as if the LED were full on; `led` is what it was.
 */
#define HR_HW_BATCH_MAX 32
#define HR_HW_CHANNELS  2

struct hr_hw_batch {
	bool worn;
	uint8_t count;
	uint32_t light[HR_HW_BATCH_MAX][HR_HW_CHANNELS];
	uint8_t led[HR_HW_BATCH_MAX];
};

/* With `always` it takes samples whether there is skin or not. */
int hr_hw_power(bool on, bool always);
/* The next lot of samples; -ENODATA when there is none waiting. */
int hr_hw_batch(struct hr_hw_batch *batch);
bool hr_hw_worn(void);
/* For the shell: the sensor's own figures, as a line of text. */
void hr_hw_describe(char *text, size_t size);
/* Called by it, from any thread, when samples wait or skin is found or lost. */
void hr_hw_event(void);

#endif
