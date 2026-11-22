#ifndef UNA_BMI270_H_
#define UNA_BMI270_H_

#include <zephyr/drivers/sensor.h>

/*
 * What the BMI270 driver in this tree (drivers/sensor/bmi270) has beyond
 * Zephyr's sensor API: the chip's own step counter, its guess at what the
 * wearer is doing, and the turn of the wrist to look at the watch.
 *
 * Acceleration and rotation are read as from any sensor, and come out in
 * the frame of the device the chip is mounted in (the node's axes).
 * The features need the accelerometer at 50 Hz; the driver runs it so for
 * as long as one of them is on, whatever rate was asked for or none.
 */

enum bmi270_channel {
	/* val1: steps counted since the counter was enabled or reset. */
	BMI270_CHAN_STEPS = SENSOR_CHAN_PRIV_START,
	/* val1: enum bmi270_activity. */
	BMI270_CHAN_ACTIVITY,
};

enum bmi270_activity {
	BMI270_ACTIVITY_STILL,
	BMI270_ACTIVITY_WALKING,
	BMI270_ACTIVITY_RUNNING,
	BMI270_ACTIVITY_UNKNOWN,
};

enum bmi270_attribute {
	/*
	 * On BMI270_CHAN_STEPS and BMI270_CHAN_ACTIVITY, val1 0 or 1: count
	 * steps, tell the activity. Both are off after start.
	 */
	BMI270_ATTR_ENABLE = SENSOR_ATTR_PRIV_START,
	/*
	 * On BMI270_CHAN_STEPS, val1 steps: BMI270_TRIG_STEP comes each time
	 * so many more are counted. The chip takes multiples of 20, up to
	 * 20460; 0 is never.
	 */
	BMI270_ATTR_STEP_WATERMARK,
	/* On BMI270_CHAN_STEPS, val1 0 or 1: BMI270_TRIG_STEP at every step. */
	BMI270_ATTR_STEP_DETECTOR,
	/* On BMI270_CHAN_STEPS: the count starts again at 0. */
	BMI270_ATTR_STEP_RESET,
};

/*
 * Besides SENSOR_TRIG_DATA_READY, SENSOR_TRIG_MOTION (any motion, set up
 * with SENSOR_ATTR_SLOPE_TH and SENSOR_ATTR_SLOPE_DUR beforehand) and
 * SENSOR_TRIG_STATIONARY (no motion). Setting a handler for a kind of
 * motion or for the wrist turns that feature on, and NULL turns it off; the
 * steps and the activity come once they are enabled, see above. Handlers
 * run in the driver's thread.
 */
enum bmi270_trigger {
	/* The step watermark was reached, or the detector saw a step. */
	BMI270_TRIG_STEP = SENSOR_TRIG_PRIV_START,
	/* The activity changed. */
	BMI270_TRIG_ACTIVITY,
	/* The wrist was turned for a look at the dial. */
	BMI270_TRIG_WRIST_WAKE,
};

#endif
