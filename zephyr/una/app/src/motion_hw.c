/*
 * BMI270 on I2C4, through the driver in this tree (drivers/sensor/bmi270).
 *
 * The chip counts the steps and knows the turn of a wrist by itself, with
 * its accelerometer at 50 Hz and the rest of it asleep: some tens of
 * microamps. It has its supply whenever the watch has, and what it has
 * counted is lost when the driver starts it again, which is at every boot.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <una/bmi270.h>

#include "motion.h"

LOG_MODULE_REGISTER(motion_hw, LOG_LEVEL_INF);

static const struct device *const imu = DEVICE_DT_GET(DT_NODELABEL(imu));

static void on_wrist(const struct device *dev, const struct sensor_trigger *trigger)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(trigger);

	motion_wrist_turned();
}

static int enable(enum bmi270_channel channel)
{
	const struct sensor_value on = {.val1 = 1};

	return sensor_attr_set(imu, (enum sensor_channel)channel,
			       (enum sensor_attribute)BMI270_ATTR_ENABLE, &on);
}

int motion_hw_start(void)
{
	/* 2 g is all a wrist asks of it, and reads finest. */
	const struct sensor_value range = {.val1 = 2};
	int ret;

	/* The driver starts here, not at boot: it takes a fifth of a second. */
	ret = device_init(imu);
	if (ret != 0 && ret != -EALREADY) {
		return ret;
	}
	ret = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_FULL_SCALE, &range);
	if (ret == 0) {
		ret = enable(BMI270_CHAN_STEPS);
	}
	if (ret == 0) {
		ret = enable(BMI270_CHAN_ACTIVITY);
	}
	return ret;
}

int motion_hw_read(uint32_t *steps, enum motion_activity *activity, int32_t accel_mg[3])
{
	struct sensor_value value, accel[3];
	int ret;

	ret = sensor_sample_fetch_chan(imu, (enum sensor_channel)BMI270_CHAN_STEPS);
	if (ret == 0) {
		ret = sensor_sample_fetch_chan(imu, SENSOR_CHAN_ACCEL_XYZ);
	}
	if (ret != 0) {
		return ret;
	}
	(void)sensor_channel_get(imu, (enum sensor_channel)BMI270_CHAN_STEPS, &value);
	*steps = value.val1;
	(void)sensor_channel_get(imu, (enum sensor_channel)BMI270_CHAN_ACTIVITY, &value);
	*activity = value.val1;
	(void)sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, accel);
	for (int i = 0; i < 3; i++) {
		accel_mg[i] = (int32_t)(sensor_value_to_micro(&accel[i]) * 1000 / SENSOR_G);
	}
	return 0;
}

int motion_hw_wrist(bool on)
{
	static const struct sensor_trigger trigger = {
		.type = (enum sensor_trigger_type)BMI270_TRIG_WRIST_WAKE,
		.chan = SENSOR_CHAN_ACCEL_XYZ,
	};

	return sensor_trigger_set(imu, &trigger, on ? on_wrist : NULL);
}
