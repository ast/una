/*
 * Copyright (c) 2026 Albin Stigo
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * What the PAH8316 driver has beyond the sensor API.
 *
 * The sensor is started by setting SENSOR_ATTR_SAMPLING_FREQUENCY to 20 and
 * stopped, its supplies cut, by setting it to 0. Started, it looks for skin,
 * and takes samples while there is some: SENSOR_CHAN_PROX reads 1 then, and
 * PAH8316_TRIG_WORN tells of the change. The samples come a second's worth
 * at a time: SENSOR_TRIG_FIFO_WATERMARK when they are there, and
 * pah8316_read_batch() for them. SENSOR_CHAN_GREEN is the last of them.
 */

#ifndef UNA_PAH8316_H_
#define UNA_PAH8316_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#define PAH8316_CHANNELS    2
#define PAH8316_BATCH_MAX   32
#define PAH8316_SAMPLE_HZ   20

enum pah8316_attribute {
	/* 1: take samples whether there is skin or not. Set before starting. */
	PAH8316_ATTR_ALWAYS = SENSOR_ATTR_PRIV_START,
};

enum pah8316_trigger {
	/* Skin was found or lost. */
	PAH8316_TRIG_WORN = SENSOR_TRIG_PRIV_START,
};

struct pah8316_batch {
	/* Uptime in ms when the batch was read: its last sample is that old. */
	int64_t uptime_ms;
	bool worn;
	uint8_t count;
	/*
	 * The chip sets its LED by itself and says how for each sample. The
	 * light is given as if the LED were full on: what was read, times
	 * 256, by the LED's setting. A step of the LED still leaves a small
	 * step in it.
	 */
	uint32_t light[PAH8316_BATCH_MAX][PAH8316_CHANNELS];
	uint8_t led[PAH8316_BATCH_MAX];
};

/*
 * The batch last read, oldest sample first. -ENODATA when there is none
 * that was not given before.
 */
int pah8316_read_batch(const struct device *dev, struct pah8316_batch *batch);

struct pah8316_stats {
	uint16_t touch;      /* the chip's reading of how near skin is */
	uint32_t batches;
	uint32_t restarts;   /* times a chip that went silent was started again */
	uint32_t bus_errors;
};

void pah8316_stats(const struct device *dev, struct pah8316_stats *stats);

#endif
