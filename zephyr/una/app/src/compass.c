/*
 * The compass: the field from the magnetometer, less the watch's own, and
 * the heading it gives once the accelerometer has said which way is down.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include "compass.h"
#include "motion.h"
#include "ui.h"

LOG_MODULE_REGISTER(compass, LOG_LEVEL_INF);

/* The sensor has a new reading every 80 ms. */
#define PERIOD_MS 160
/* A heading older than this is not where the watch points now. */
#define FRESH_MS  1000
/* Each reading moves what is used half the way: the jitter goes, a turn shows at once. */
#define SMOOTH    2
/* So many readings failed in a row: it is started again. */
#define FAILURES_TO_RESTART 5
/*
 * Shaken, the accelerometer does not read which way is down: outside this
 * much of 1 g the heading stays what it was.
 */
#define STEADY_MG 200

/*
 * Calibration. An axis has been turned through the field when it has seen
 * it both ways: a span of twice the field, of which the earth has 25 uT at
 * the least. It is done when the axis that has seen least has seen nearly
 * what the one that has seen most has.
 */
#define SPAN_MIN_NT   40000
#define SPAN_SHARE    85 /* percent */

static atomic_t wanted;
static K_SEM_DEFINE(wake, 0, 1);

static struct k_spinlock lock;
static struct compass_reading last;
static int64_t last_ms;
static bool have_last;
static struct compass_status state;

/* The watch's own field, and whether it has been found. */
static int32_t offset_nt[3];
static uint8_t calibrated;

/* A calibration under way: asked for, and the least and most each axis has read. */
static atomic_t calibrate; /* 1 begin, 2 end */
static int32_t seen_min[3], seen_max[3];

void compass_want(enum compass_user user, bool on)
{
	if (on) {
		atomic_set_bit(&wanted, user);
	} else {
		atomic_clear_bit(&wanted, user);
	}
	k_sem_give(&wake);
}

bool compass_read(struct compass_reading *reading)
{
	bool fresh;

	K_SPINLOCK(&lock) {
		*reading = last;
		fresh = have_last && k_uptime_get() - last_ms < FRESH_MS;
	}
	return fresh;
}

void compass_status(struct compass_status *status)
{
	K_SPINLOCK(&lock) {
		*status = state;
	}
}

void compass_calibrate(bool on)
{
	atomic_set(&calibrate, on ? 1 : 2);
	k_sem_give(&wake);
}

static int compass_setting_set(const char *name, size_t len, settings_read_cb read_cb,
			       void *cb_arg)
{
	if (settings_name_steq(name, "offset", NULL) && len == sizeof(offset_nt)) {
		if (read_cb(cb_arg, offset_nt, sizeof(offset_nt)) < 0) {
			return -EIO;
		}
		calibrated = 1;
		return 0;
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(compass, "compass", NULL, compass_setting_set, NULL, NULL);

/* The C library here has no mathematics. */
static float root(float x)
{
	float r = x > 1.0f ? x : 1.0f;

	if (x <= 0.0f) {
		return 0.0f;
	}
	for (int i = 0; i < 24; i++) {
		r = (r + x / r) / 2;
	}
	return r;
}

/* The angle of (x, y) from the x axis towards y, 0 to 360 degrees; good to a quarter. */
static float angle(float y, float x)
{
	float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
	float z, a;

	if (ax == 0.0f && ay == 0.0f) {
		return 0.0f;
	}
	z = ax > ay ? ay / ax : ax / ay;
	/* The arc tangent up to 45 degrees, bent to fit. */
	a = z * (45.0f + 15.64f * (1.0f - z));
	if (ay > ax) {
		a = 90.0f - a;
	}
	if (x < 0) {
		a = 180.0f - a;
	}
	return y < 0 ? 360.0f - a : a;
}

/*
 * The heading from the field m and from a, which points up. What of the
 * field lies level points north; east is at a right angle to that and to
 * up. Twelve o'clock is the y axis: how much of it is north, how much east.
 */
static void heading(const float m[3], const float a[3], struct compass_reading *out)
{
	float aa = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
	float ma = m[0] * a[0] + m[1] * a[1] + m[2] * a[2];
	float up = root(aa);
	/* The y of north, times aa, and of east, times up: so east times up again. */
	float north = m[1] * aa - ma * a[1];
	float east = (m[2] * a[0] - m[0] * a[2]) * up;

	out->heading_deg = (int)(angle(east, north) + 0.5f) % 360;
	out->tilt_deg = (int)(angle(root(a[0] * a[0] + a[1] * a[1]), a[2]) + 0.5f);
	out->field_nt = (int32_t)root(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
}

static bool set_power(bool on)
{
	int ret = compass_hw_power(on);

	K_SPINLOCK(&lock) {
		state.on = on && ret == 0;
		state.readings = 0;
		have_last = false;
		if (on) {
			state.present = ret == 0;
		}
	}
	if (on && ret != 0) {
		LOG_ERR("no sensor: %d", ret);
		/* Whoever wanted it must ask again. */
		atomic_clear(&wanted);
	} else {
		LOG_INF("sensor %s (wanted by %lx)", on ? "on" : "off", (long)atomic_get(&wanted));
	}
	ui_refresh();
	return on && ret == 0;
}

/* Returns the percent of a calibration that is done, and ends it at a hundred. */
static int calibration(const int32_t raw[3], bool *calibrating)
{
	int32_t least = INT32_MAX, most = 0;
	int progress;

	switch (atomic_clear(&calibrate)) {
	case 1:
		for (int i = 0; i < 3; i++) {
			seen_min[i] = seen_max[i] = raw[i];
		}
		*calibrating = true;
		LOG_INF("calibration begun");
		break;
	case 2:
		if (*calibrating) {
			*calibrating = false;
			LOG_INF("calibration given up");
		}
		break;
	}
	if (!*calibrating) {
		return 0;
	}
	for (int i = 0; i < 3; i++) {
		seen_min[i] = MIN(seen_min[i], raw[i]);
		seen_max[i] = MAX(seen_max[i], raw[i]);
		least = MIN(least, seen_max[i] - seen_min[i]);
		most = MAX(most, seen_max[i] - seen_min[i]);
	}
	progress = (int)((int64_t)least * 100 / MAX(most / 100 * SPAN_SHARE, SPAN_MIN_NT));
	if (progress < 100) {
		return progress;
	}
	K_SPINLOCK(&lock) {
		for (int i = 0; i < 3; i++) {
			offset_nt[i] = (seen_min[i] + seen_max[i]) / 2;
		}
		calibrated = 1;
	}
	settings_save_one("compass/offset", offset_nt, sizeof(offset_nt));
	*calibrating = false;
	LOG_INF("calibrated: the watch's own field is %d %d %d nT", (int)offset_nt[0],
		(int)offset_nt[1], (int)offset_nt[2]);
	return 100;
}

static void sample(void)
{
	static float field[3], up[3];
	static bool calibrating;
	static int in_a_row;
	struct compass_reading reading = {0};
	int32_t raw[3], mg[3] = {0, 0, 1000};
	int progress, ret;
	bool level_assumed, steady, first;

	ret = compass_hw_sample(raw);
	if (ret != 0) {
		K_SPINLOCK(&lock) {
			state.failures++;
		}
		if (++in_a_row == FAILURES_TO_RESTART) {
			LOG_WRN("reading fails (%d): started again", ret);
			in_a_row = 0;
			(void)compass_hw_power(false);
			(void)set_power(true);
		}
		return;
	}
	in_a_row = 0;
	level_assumed = motion_hw_accel(mg) != 0;
	if (level_assumed) {
		mg[0] = 0;
		mg[1] = 0;
		mg[2] = 1000;
	}
	steady = abs((int)root((float)mg[0] * mg[0] + (float)mg[1] * mg[1] +
			       (float)mg[2] * mg[2]) - 1000) < STEADY_MG;
	progress = calibration(raw, &calibrating);

	K_SPINLOCK(&lock) {
		first = state.readings == 0;
	}
	for (int i = 0; i < 3; i++) {
		float m = raw[i] - offset_nt[i];

		field[i] = first ? m : field[i] + (m - field[i]) / SMOOTH;
		if (first || steady) {
			up[i] = first ? mg[i] : up[i] + (mg[i] - up[i]) / SMOOTH;
		}
	}
	heading(field, up, &reading);
	reading.level_assumed = level_assumed;

	K_SPINLOCK(&lock) {
		/* While the wrist is shaken the heading is the one from before. */
		if (steady || !have_last) {
			last = reading;
		}
		last_ms = k_uptime_get();
		have_last = true;
		state.readings++;
		state.calibrated = calibrated;
		state.calibrating = calibrating;
		state.progress = progress;
		memcpy(state.raw_nt, raw, sizeof(raw));
		memcpy(state.offset_nt, offset_nt, sizeof(offset_nt));
	}
}

static void compass_thread(void *p1, void *p2, void *p3)
{
	bool on = false;
	int64_t next_ms = 0;

	while (true) {
		bool want = atomic_get(&wanted) != 0;

		if (want != on) {
			on = set_power(want);
			next_ms = k_uptime_get() + PERIOD_MS;
		}
		if (!on) {
			k_sem_take(&wake, K_FOREVER);
			continue;
		}
		if (k_uptime_get() >= next_ms) {
			sample();
			on = state.on;
			next_ms += PERIOD_MS;
			if (next_ms < k_uptime_get()) {
				next_ms = k_uptime_get() + PERIOD_MS;
			}
		}
		k_sem_take(&wake, K_TIMEOUT_ABS_MS(next_ms));
	}
}
K_THREAD_DEFINE(compass_tid, 2048, compass_thread, NULL, NULL, NULL, 9, 0, 500);

/* compass [on|off|calibrate] */
static int cmd_compass(const struct shell *sh, size_t argc, char **argv)
{
	struct compass_reading r;
	struct compass_status s;

	if (argc > 1 && strcmp(argv[1], "calibrate") == 0) {
		compass_want(COMPASS_FOR_SHELL, true);
		compass_calibrate(true);
		return 0;
	}
	if (argc > 1) {
		compass_want(COMPASS_FOR_SHELL, strcmp(argv[1], "on") == 0);
		return 0;
	}
	compass_status(&s);
	shell_print(sh, "%s, %s, %u readings, %u failures", s.present ? "present" : "not seen",
		    s.on ? "on" : "off", s.readings, s.failures);
	if (s.calibrating) {
		shell_print(sh, "calibrating: %d %%", s.progress);
	} else {
		shell_print(sh, "%s", s.calibrated ? "calibrated" : "not calibrated");
	}
	if (s.on) {
		shell_print(sh, "sensor %d %d %d nT, the watch's own %d %d %d nT", (int)s.raw_nt[0],
			    (int)s.raw_nt[1], (int)s.raw_nt[2], (int)s.offset_nt[0],
			    (int)s.offset_nt[1], (int)s.offset_nt[2]);
	}
	if (compass_read(&r)) {
		shell_print(sh, "heading %d, tilt %d%s, field %d nT", r.heading_deg, r.tilt_deg,
			    r.level_assumed ? " (assumed: no accelerometer)" : "",
			    (int)r.field_nt);
	}
	return 0;
}
SHELL_CMD_ARG_REGISTER(compass, NULL,
		       "Compass state, or turn it on or off, or calibrate it: "
		       "compass [on|off|calibrate]",
		       cmd_compass, 1, 1);
