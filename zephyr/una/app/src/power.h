#ifndef UNA_POWER_H_
#define UNA_POWER_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/atomic.h>

struct power_status {
	int percent;
	int millivolts;
	int microamps; /* average; positive while charging */
	int remaining_mah;
	int full_mah;
	bool usb_power;
	uint8_t charger_state; /* PCA9420 CHG_STATUS_2[2:0] */
	unsigned int charge_limit_ma;
};

int power_status(struct power_status *st);

/* The power thread's latest reading, without touching the bus. */
void power_last(struct power_status *st);

/* Does not return when it works and there is no USB power. */
int power_off(void);

/* Usage counters for the telemetry line, kept by the UI. */
extern atomic_t usage_key_presses;
extern atomic_t usage_backlight_ms;

/* src/usb.c */
void usb_set_enabled(bool on);

#endif
