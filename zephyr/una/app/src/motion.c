/*
 * The steps of the day and the wrist that lights the screen: what is made
 * of the accelerometer's counting.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include "clock.h"
#include "motion.h"
#include "ui.h"

LOG_MODULE_REGISTER(motion, LOG_LEVEL_INF);

/* The count is the sensor's; asking for it is all that wakes the watch. */
#define PERIOD_S 60

static K_SEM_DEFINE(wake, 0, 1);
static K_SEM_DEFINE(polled, 0, 1);

static struct k_spinlock lock;
static struct motion_status state = {.activity = MOTION_UNKNOWN};
static int32_t accel_mg[3];
static atomic_t wrist_turns;

static uint8_t wrist_light = 1;

void motion_status(struct motion_status *status)
{
	K_SPINLOCK(&lock) {
		*status = state;
	}
	status->wrist_turns = atomic_get(&wrist_turns);
}

void motion_poll(void)
{
	k_sem_give(&wake);
}

bool motion_wrist_light(void)
{
	return wrist_light;
}

void motion_set_wrist_light(bool on)
{
	wrist_light = on;
	settings_save_one("motion/wrist", &wrist_light, sizeof(wrist_light));
	k_sem_give(&wake);
}

static int motion_setting_set(const char *name, size_t len, settings_read_cb read_cb,
			      void *cb_arg)
{
	if (settings_name_steq(name, "wrist", NULL) && len == sizeof(wrist_light)) {
		return read_cb(cb_arg, &wrist_light, sizeof(wrist_light)) < 0 ? -EIO : 0;
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(motion, "motion", NULL, motion_setting_set, NULL, NULL);

void motion_wrist_turned(void)
{
	atomic_inc(&wrist_turns);
	ui_light();
}

/* The day, as a number that changes at midnight; -1 while the clock is not set. */
static int today(void)
{
	struct tm now;

	return clock_local(&now) ? now.tm_year * 366 + now.tm_yday : -1;
}

static void poll(void)
{
	static uint32_t last_steps;
	static int day = -1;
	enum motion_activity activity;
	uint32_t steps, ended = 0;
	int32_t mg[3];
	int now = today();
	int ret = motion_hw_read(&steps, &activity, mg);

	K_SPINLOCK(&lock) {
		if (ret != 0) {
			state.failures++;
		} else {
			/* What was walked since the last look belongs to the day it was in. */
			state.steps_today += steps - last_steps;
			state.steps = steps;
			state.activity = activity;
			memcpy(accel_mg, mg, sizeof(accel_mg));
			last_steps = steps;
		}
		if (day != -1 && now != -1 && now != day) {
			ended = state.steps_today;
			state.steps_today = 0;
		}
	}
	if (now != -1) {
		if (day != -1 && now != day) {
			LOG_INF("%u steps in the day that ended", ended);
		}
		day = now;
	}
	if (ret != 0) {
		LOG_WRN("reading fails: %d", ret);
	}
}

static void motion_thread(void *p1, void *p2, void *p3)
{
	bool wrist = false;
	int ret = motion_hw_start();

	if (ret != 0) {
		LOG_ERR("no sensor: %d", ret);
		return;
	}
	K_SPINLOCK(&lock) {
		state.present = true;
	}
	LOG_INF("sensor started");

	while (true) {
		if (wrist != (bool)wrist_light) {
			ret = motion_hw_wrist(wrist_light);
			if (ret != 0) {
				LOG_WRN("wrist %s: %d", wrist_light ? "on" : "off", ret);
			}
			wrist = wrist_light;
		}
		poll();
		k_sem_give(&polled);
		k_sem_take(&wake, K_SECONDS(PERIOD_S));
	}
}
/* After the settings are read, and off the path of the boot. */
K_THREAD_DEFINE(motion_tid, 2048, motion_thread, NULL, NULL, NULL, 9, 0, 1000);

/* motion [wrist on|off] */
static int cmd_motion(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const activities[] = {"still", "walking", "running", "unknown"};
	struct motion_status s;
	int32_t mg[3];

	if (argc == 3 && strcmp(argv[1], "wrist") == 0) {
		motion_set_wrist_light(strcmp(argv[2], "on") == 0);
		return 0;
	}
	if (argc != 1) {
		return -EINVAL;
	}
	k_sem_reset(&polled);
	motion_poll();
	(void)k_sem_take(&polled, K_SECONDS(1));
	motion_status(&s);
	K_SPINLOCK(&lock) {
		memcpy(mg, accel_mg, sizeof(mg));
	}
	shell_print(sh, "%s, %u failures", s.present ? "present" : "not seen", s.failures);
	shell_print(sh, "%u steps today, %u since boot, %s", s.steps_today, s.steps,
		    activities[s.activity]);
	shell_print(sh, "wrist lights the screen: %s, %u turns since boot",
		    wrist_light ? "yes" : "no", s.wrist_turns);
	shell_print(sh, "%d %d %d mg", (int)mg[0], (int)mg[1], (int)mg[2]);
	return 0;
}
SHELL_CMD_ARG_REGISTER(motion, NULL,
		       "Steps, activity, acceleration; \"motion wrist on|off\": the wrist "
		       "lights the screen",
		       cmd_motion, 1, 2);
