#ifndef UNA_HR_ALG_H_
#define UNA_HR_ALG_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * The pulse, from the light that comes back through the skin and the way
 * the wrist is moved. Nothing of the watch is in it: the same file is built
 * for the computer, to be run over what was recorded (scripts/hr-replay).
 */

#define HR_ALG_HZ 20

struct hr_alg_sample {
	uint32_t light;      /* both channels together, scaled by the LED */
	uint8_t led;         /* the LED's setting: a change of it is a step in the light */
	int16_t accel_mg[3];
};

struct hr_alg_out {
	uint16_t bpm10;  /* tenths of a beat a minute; 0 when there is none to give */
	uint8_t quality; /* 0 to 100: how much of what moves in the light is the pulse */
	uint16_t raw_bpm10; /* this second's own answer, before it is held to the others */
	bool moving;
};

void hr_alg_reset(void);

/* Samples as they come, any number at a time; an answer after each lot. */
void hr_alg_feed(const struct hr_alg_sample *samples, unsigned int count, struct hr_alg_out *out);

#endif
