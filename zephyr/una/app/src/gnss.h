#ifndef UNA_GNSS_H_
#define UNA_GNSS_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * The GNSS receiver: on while anyone wants it, and what it last said.
 */

/* Who may want it on. */
enum gnss_user {
	GNSS_FOR_RECORD,
	GNSS_FOR_PAGE,
	GNSS_FOR_SHELL,
};

/* Returns at once; turning it on takes a second or two. */
void gnss_want(enum gnss_user user, bool on);
bool gnss_wanted(enum gnss_user user);

struct gnss_fix {
	int64_t latitude; /* nanodegrees */
	int64_t longitude;
	int32_t altitude_mm;
	uint32_t speed_mm_s;
	int64_t at_ms; /* uptime */
};

/* False when there is none, or none fresh enough to use. */
bool gnss_fix(struct gnss_fix *fix);

#define GNSS_SNR_N 12

struct gnss_status {
	bool present;      /* there is a receiver */
	bool on;
	bool fix;
	int used;          /* satellites in the fix */
	int seen;          /* satellites heard */
	int hdop_tenths;
	uint8_t snr[GNSS_SNR_N]; /* the strongest, in dB-Hz, strongest first */
	uint32_t on_s;     /* since it was turned on */
	int32_t first_fix_s; /* how long the first fix took; -1 before it */
	uint32_t sentences;
};

void gnss_status(struct gnss_status *status);

#endif
