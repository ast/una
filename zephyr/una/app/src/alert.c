/*
 * Alerts: a beep-and-buzz pattern, and the screen shown while it plays.
 */

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>

#include "alert.h"
#include "clock.h"
#include "ui.h"

LOG_MODULE_REGISTER(alert, LOG_LEVEL_INF);

/* Two short pulses a second. */
static const struct {
	uint16_t ms;
	bool on;
} pattern[] = {{150, true}, {100, false}, {150, true}, {600, false}};

#define PATTERN_MS 1000

static uint8_t sound = 1;
static uint8_t vibration = 1;

/*
 * A pattern is asked for from any thread and played by the work alone: the
 * steps to play from the beginning, 0 to stop, -1 when nothing is asked.
 * Cancelling the work instead would, if it is running just then, have the
 * kernel refuse every schedule until it returns, its own for the next step
 * among them, and whatever was on would stay on.
 */
static atomic_t asked = ATOMIC_INIT(-1);

/* The work's own: steps still to play, the one that is next, and when. */
static int steps_left;
static int step;
static int64_t step_at;

static void play_step(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(step_work, play_step);

static void play_step(struct k_work *work)
{
	int fresh = atomic_set(&asked, -1);
	int64_t now = k_uptime_get();
	bool on;

	if (fresh >= 0) {
		steps_left = fresh;
		step = 0;
	} else if (steps_left > 0 && now < step_at) {
		/* Woken for a pattern that an earlier run already took. */
		k_work_schedule(&step_work, K_MSEC(step_at - now));
		return;
	}
	if (steps_left <= 0) {
		buzzer_set(false);
		motor_set(false);
		return;
	}
	steps_left--;
	on = pattern[step].on;
	buzzer_set(on && sound);
	motor_set(on && vibration);
	step_at = k_uptime_get() + pattern[step].ms;
	k_work_schedule(&step_work, K_MSEC(pattern[step].ms));
	step = (step + 1) % ARRAY_SIZE(pattern);
}

/* Whether what was last asked for is the ring that the ring screen shows. */
static atomic_t ringing;

static void start(int steps)
{
	atomic_set(&ringing, 0);
	atomic_set(&asked, steps);
	k_work_reschedule(&step_work, K_NO_WAIT);
}

void alert_stop(void)
{
	start(0);
}

void alert_blip(void)
{
	start(2);
}

/* The screen */

static const char *ring_title;
static char ring_time[6];

static void ring_enter(void)
{
	lv_label_set_text(ui_label(&lv_font_montserrat_48, 0xffffff, -20), ring_time);
	lv_label_set_text(ui_label(&font_text_20, 0xffff00, 30), ring_title);
	lv_label_set_text(ui_label(&lv_font_montserrat_14, 0xaaaaaa, 76), "any button: dismiss");
}

static uint32_t ring_update(void)
{
	return UINT32_MAX;
}

static bool ring_key(enum ui_key key, bool held)
{
	ARG_UNUSED(key);
	ARG_UNUSED(held);

	ui_pop();
	return true;
}

/*
 * Leaving is what silences it, so that holding BACK, which the screen does
 * not see, does too. A blip that has come since is left alone.
 */
static void ring_leave(void)
{
	if (atomic_get(&ringing)) {
		alert_stop();
	}
}

static const struct ui_screen ring_screen = {
	.name = "ring",
	.enter = ring_enter,
	.update = ring_update,
	.key = ring_key,
	.leave = ring_leave,
};

void alert_ring(const char *title, int seconds)
{
	struct tm now;

	LOG_INF("ring: %s", title);
	ring_title = title;
	if (clock_local(&now)) {
		snprintk(ring_time, sizeof(ring_time), "%02d:%02d", now.tm_hour, now.tm_min);
	} else {
		ring_time[0] = '\0';
	}
	start(seconds * 1000 / PATTERN_MS * ARRAY_SIZE(pattern));
	atomic_set(&ringing, 1);
	ui_show(&ring_screen);
}

/* Settings */

bool alert_sound(void)
{
	return sound;
}

void alert_set_sound(bool on)
{
	sound = on;
	settings_save_one("alert/sound", &sound, sizeof(sound));
}

bool alert_vibration(void)
{
	return vibration;
}

void alert_set_vibration(bool on)
{
	vibration = on;
	settings_save_one("alert/vibration", &vibration, sizeof(vibration));
}

static int alert_setting_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t *value = settings_name_steq(name, "sound", NULL)       ? &sound
			 : settings_name_steq(name, "vibration", NULL) ? &vibration
								       : NULL;

	if (value == NULL || len != sizeof(*value)) {
		return -ENOENT;
	}
	return read_cb(cb_arg, value, sizeof(*value)) < 0 ? -EIO : 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(alert, "alert", NULL, alert_setting_set, NULL, NULL);

/* alert ring [seconds] | alert stop */
static int cmd_ring(const struct shell *sh, size_t argc, char **argv)
{
	alert_ring("Test", argc > 1 ? atoi(argv[1]) : 5);
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	alert_stop();
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(alert_cmds,
	SHELL_CMD_ARG(ring, NULL, "Ring for [seconds] (default 5)", cmd_ring, 1, 1),
	SHELL_CMD(stop, NULL, "Silence", cmd_stop),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(alert, &alert_cmds, "Beep and vibrate", NULL);
