#ifndef UNA_BARO_H_
#define UNA_BARO_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * The barometer: on while anyone wants it, read once a second.
 */

/* Who may want it on. */
enum baro_user {
	BARO_FOR_RECORD,
	BARO_FOR_PAGE,
	BARO_FOR_SHELL,
};

/* Returns at once; the first reading is there within a second or two. */
void baro_want(enum baro_user user, bool on);
bool baro_wanted(enum baro_user user);

struct baro_reading {
	int32_t pressure_pa;
	int32_t temperature_cc; /* hundredths of a degree, of the sensor itself */
	/*
	 * What the pressure is as a height in the standard atmosphere, evened
	 * out over a few seconds. Good for differences; as a height above the
	 * sea it is off by what the weather is, a hundred metres and more.
	 */
	int32_t altitude_mm;
	int64_t at_ms; /* uptime */
};

/* False when there is none, or none fresh enough to use. */
bool baro_read(struct baro_reading *reading);

struct baro_status {
	bool present; /* there is a sensor; known once it has been on */
	bool on;
	uint32_t readings; /* since it was turned on */
	uint32_t failures; /* since boot */
};

void baro_status(struct baro_status *status);

/*
 * The sensor itself: src/baro_hw.c, or the simulator's stand-in. Pressure
 * in Pa, temperature in hundredths of a degree.
 */
int baro_hw_power(bool on);
int baro_hw_sample(int32_t *pressure_pa, int32_t *temperature_cc);

#endif
