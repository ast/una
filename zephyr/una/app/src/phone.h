#ifndef UNA_PHONE_H_
#define UNA_PHONE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/*
 * The phone, through Gadgetbridge, which takes the watch for a Bangle.js:
 * lines of text over the serial line. It sends the time and what it is
 * notified of; the watch tells it its version and battery.
 */

/* From the radio: bytes as they arrive, and the line opening and closing. */
void phone_rx(const uint8_t *data, size_t len);
void phone_connected(void);
void phone_disconnected(void);

/* The battery, whenever it is read: passed on when it has changed. */
void phone_battery(int percent, bool charging, int millivolts);

/* What the phone was notified of, text in UTF-8. */
struct phone_note {
	int32_t id;
	time_t at;      /* UTC; 0 when the clock was not set */
	char source[24]; /* the app */
	char title[64];
	char body[280];
};

/* The latest are kept, the newest first. */
#define PHONE_NOTES 8
int phone_notes(void);
bool phone_note(int index, struct phone_note *note);
/* Goes up with every change to them. */
uint32_t phone_notes_version(void);

struct phone_status {
	uint32_t lines;      /* received since boot */
	uint32_t unknown;    /* of them, not understood */
	time_t time_set_at;  /* when the phone last set the clock; 0 for never */
};

void phone_status(struct phone_status *status);

#endif
