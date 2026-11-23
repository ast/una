#ifndef UNA_BLUETOOTH_H_
#define UNA_BLUETOOTH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The radio: on or off as set, advertising until the phone connects, and a
 * serial line to the phone (the Nordic UART service) once it has and has
 * shown itself to be paired. A phone pairs by the passkey the watch shows.
 */

/* Stored. Returns at once; the radio follows within a few seconds. */
void bluetooth_set(bool on);
bool bluetooth_is_set(void);

/* Forget every phone paired. Returns at once. */
void bluetooth_forget(void);

enum bluetooth_state {
	BLUETOOTH_OFF,       /* chip held in reset */
	BLUETOOTH_STARTING,
	BLUETOOTH_ADVERTISING,
	BLUETOOTH_CONNECTED,
	BLUETOOTH_FAILED,    /* the chip did not start */
};

struct bluetooth_status {
	enum bluetooth_state state;
	bool secure;          /* the link is encrypted with a paired phone's key */
	bool listening;       /* the phone has opened the serial line */
	uint8_t bonds;
	/* Links lost because the phone was no longer heard (reason 0x08). */
	uint32_t timeouts;        /* phones paired */
	char peer[18];        /* its address, "" when not connected */
	char own[18];
	uint16_t interval_ms; /* connection interval */
	uint16_t latency;
	uint16_t mtu;
	uint32_t connected_s; /* for how long */
	uint32_t connects;    /* since boot */
	uint32_t resets;      /* times the radio was started over, since boot */
	uint32_t rx_bytes;
	uint32_t tx_bytes;
	int8_t rssi;          /* dBm as the chip last heard the phone; 0 when not known */
	int rssi_status;      /* what the chip answered when asked: 0, its status, or -errno */
};

void bluetooth_status(struct bluetooth_status *status);

/* To the phone, in as many notifications as it takes. False when nobody listens. */
bool bluetooth_send(const void *data, size_t len);

#endif
