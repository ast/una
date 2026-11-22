/*
 * BMM350 on I2C4, through the driver in this tree (drivers/sensor/bmm350).
 *
 * It has its supply whenever the watch has; off is its suspend, at a few
 * microamps. On, it measures 12.5 times a second, each the mean of four,
 * as the stock firmware has it average: a third of a milliamp or so.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>

#include "compass.h"

static const struct device *const mag = DEVICE_DT_GET(DT_NODELABEL(magnetometer));

int compass_hw_power(bool on)
{
	const struct sensor_value average = {.val1 = 4};
	const struct sensor_value rate = {.val1 = on ? 12 : 0, .val2 = on ? 500000 : 0};
	int ret;

	/* The driver starts here, not at boot: it takes a tenth of a second. */
	ret = device_init(mag);
	if (ret != 0 && ret != -EALREADY) {
		return ret;
	}
	if (!device_is_ready(mag)) {
		return -ENODEV;
	}
	if (on) {
		ret = sensor_attr_set(mag, SENSOR_CHAN_MAGN_XYZ, SENSOR_ATTR_OVERSAMPLING,
				      &average);
		if (ret != 0) {
			return ret;
		}
	}
	return sensor_attr_set(mag, SENSOR_CHAN_MAGN_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &rate);
}

int compass_hw_sample(int32_t field_nt[3])
{
	struct sensor_value gauss[3];
	int ret = sensor_sample_fetch(mag);

	if (ret != 0) {
		return ret;
	}
	(void)sensor_channel_get(mag, SENSOR_CHAN_MAGN_XYZ, gauss);
	for (int i = 0; i < 3; i++) {
		/* A Gauss is 100 uT. */
		field_nt[i] = (int32_t)(sensor_value_to_micro(&gauss[i]) / 10);
	}
	return 0;
}
