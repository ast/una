/*
 * PAH8316 on I2C2, through the driver in this tree (drivers/sensor/pah8316).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>

#include <una/pah8316.h>

#include "hr.h"

BUILD_ASSERT(HR_HW_BATCH_MAX == PAH8316_BATCH_MAX && HR_HW_CHANNELS == PAH8316_CHANNELS);

static const struct device *const ppg = DEVICE_DT_GET(DT_NODELABEL(ppg));

static void on_event(const struct device *dev, const struct sensor_trigger *trigger)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(trigger);

	hr_hw_event();
}

int hr_hw_power(bool on, bool always)
{
	static const struct sensor_trigger batch = {
		.type = SENSOR_TRIG_FIFO_WATERMARK,
		.chan = SENSOR_CHAN_GREEN,
	};
	static const struct sensor_trigger worn = {
		.type = (enum sensor_trigger_type)PAH8316_TRIG_WORN,
		.chan = SENSOR_CHAN_PROX,
	};
	struct sensor_value value = {.val1 = 0};
	int ret;

	if (!device_is_ready(ppg)) {
		return -ENODEV;
	}
	/* Off first, whatever it was: the way it runs is set while it is off. */
	ret = sensor_attr_set(ppg, SENSOR_CHAN_ALL, SENSOR_ATTR_SAMPLING_FREQUENCY, &value);
	if (!on || ret != 0) {
		return ret;
	}
	(void)sensor_trigger_set(ppg, &batch, on_event);
	(void)sensor_trigger_set(ppg, &worn, on_event);
	value.val1 = always;
	ret = sensor_attr_set(ppg, SENSOR_CHAN_ALL, (enum sensor_attribute)PAH8316_ATTR_ALWAYS,
			      &value);
	if (ret == 0) {
		value.val1 = PAH8316_SAMPLE_HZ;
		ret = sensor_attr_set(ppg, SENSOR_CHAN_ALL, SENSOR_ATTR_SAMPLING_FREQUENCY, &value);
	}
	return ret;
}

int hr_hw_batch(struct hr_hw_batch *batch)
{
	static struct pah8316_batch raw;
	int ret = pah8316_read_batch(ppg, &raw);

	if (ret != 0) {
		return ret;
	}
	batch->worn = raw.worn;
	batch->count = raw.count;
	memcpy(batch->light, raw.light, sizeof(batch->light));
	memcpy(batch->led, raw.led, sizeof(batch->led));
	return 0;
}

bool hr_hw_worn(void)
{
	struct sensor_value value = {0};

	(void)sensor_channel_get(ppg, SENSOR_CHAN_PROX, &value);
	return value.val1 != 0;
}

void hr_hw_describe(char *text, size_t size)
{
	struct pah8316_stats stats;

	pah8316_stats(ppg, &stats);
	snprintf(text, size, "touch %u, %u batches from the chip, %u restarts, %u bus errors",
		 stats.touch, stats.batches, stats.restarts, stats.bus_errors);
}
