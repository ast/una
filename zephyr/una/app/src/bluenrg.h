#ifndef UNA_BLUENRG_H_
#define UNA_BLUENRG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/*
 * The BlueNRG-2N radio chip. It runs the whole Bluetooth stack itself and is
 * told what to do in commands over SPI: HCI's own, and ST's for its GAP and
 * GATT layers (the ACI). What happens comes back as events.
 */

/* Out of reset, and wait until it says it has started. 0 or -errno. */
int bluenrg_start(void);
/* Into reset, where it stays and draws nothing. */
void bluenrg_stop(void);

#define BLUENRG_OP(ogf, ocf) ((uint16_t)(((ogf) << 10) | (ocf)))
#define BLUENRG_ACI(ocf)     BLUENRG_OP(0x3f, ocf)

/*
 * Send a command and wait for the chip's answer. Returns the answer's status
 * byte (0 is success, the rest are the chip's error codes) or -errno when
 * there was no answer. What follows the status goes to rsp, as much as fits.
 * Not from the thread that takes the events off bluenrg_event() while it has
 * none to take: any thread else.
 */
int bluenrg_cmd(uint16_t opcode, const void *params, size_t len, void *rsp, size_t rsp_size);

#define BLUENRG_EVENT_MAX 260

enum {
	BLUENRG_EVENT_WAKE = 0x00, /* not the chip's: see bluenrg_wake() */
	BLUENRG_EVENT_DISCONNECTED = 0x05,
	BLUENRG_EVENT_ENCRYPTION = 0x08,
	BLUENRG_EVENT_LE = 0x3e,     /* code is the subevent */
	BLUENRG_EVENT_VENDOR = 0xff, /* code is ST's event code */
};

struct bluenrg_event {
	uint8_t event;
	uint16_t code;
	uint16_t len;
	uint8_t data[BLUENRG_EVENT_MAX]; /* after the subevent or event code */
};

/* The next event; false when none came in time. */
bool bluenrg_event(struct bluenrg_event *event, k_timeout_t timeout);
/* Any thread: have bluenrg_event() return a BLUENRG_EVENT_WAKE. */
void bluenrg_wake(void);

#endif
