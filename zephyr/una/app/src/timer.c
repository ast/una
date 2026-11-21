/*
 * Countdown timer, set in minutes. Keeps running when another screen is
 * shown and rings when it is done.
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "alert.h"
#include "ui.h"

#define MAX_MINUTES 180
#define RING_S      30

static int32_t minutes = 5;
static bool running;
static bool editing; /* inside: UP and DOWN set the minutes */
static int64_t ends_at;

static lv_obj_t *time_label;
static lv_obj_t *hint_label;

static void done(struct k_work *work)
{
	ARG_UNUSED(work);

	running = false;
	alert_ring("Timer", RING_S);
}
static K_WORK_DELAYABLE_DEFINE(done_work, done);

static void timer_enter(void)
{
	ui_title("TIMER");
	editing = false;
	time_label = ui_label(&lv_font_montserrat_48, 0xffffff, -14);
	hint_label = ui_label(&lv_font_montserrat_14, 0xaaaaaa, 60);
}

static uint32_t timer_update(void)
{
	int64_t left_ms = ends_at - k_uptime_get();
	int s;

	if (!running || left_ms <= 0) {
		lv_label_set_text_fmt(time_label, "%02d:00", (int)minutes);
		lv_obj_set_style_text_color(time_label,
					    lv_color_hex(editing ? 0x00ffff : 0xffffff), 0);
		lv_label_set_text(hint_label, editing ? "" : "hold: set");
		/* Running with nothing left: about to ring. */
		return running ? 100 : UINT32_MAX;
	}
	/* Rounded up: the display reaches 00:00 as the timer rings. */
	s = (left_ms + 999) / 1000;
	if (s >= 3600) {
		lv_label_set_text_fmt(time_label, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	} else {
		lv_label_set_text_fmt(time_label, "%02d:%02d", s / 60, s % 60);
	}
	lv_obj_set_style_text_color(time_label, lv_color_hex(0x00ff00), 0);
	lv_label_set_text(hint_label, "");
	return (left_ms - 1) % 1000 + 1;
}

/* SELECT starts and stops; held, it goes in to set the minutes. */
static bool timer_key(enum ui_key key, bool held)
{
	if (editing) {
		if (key == UI_KEY_UP || key == UI_KEY_DOWN) {
			/* Held: tens. */
			int step = (held ? 10 : 1) * (key == UI_KEY_UP ? 1 : -1);

			minutes = CLAMP(minutes + step, 1, MAX_MINUTES);
		} else if (!held) {
			editing = false;
			settings_save_one("timer/minutes", &minutes, sizeof(minutes));
		}
		return true;
	}
	if (key != UI_KEY_SELECT) {
		return false;
	}
	if (held) {
		editing = !running;
	} else if (running) {
		k_work_cancel_delayable(&done_work);
		running = false;
	} else {
		ends_at = k_uptime_get() + minutes * 60000;
		k_work_reschedule(&done_work, K_TIMEOUT_ABS_MS(ends_at));
		running = true;
	}
	return true;
}

static int timer_setting_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "minutes", NULL) && len == sizeof(minutes)) {
		return read_cb(cb_arg, &minutes, sizeof(minutes)) < 0 ? -EIO : 0;
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(timer, "timer", NULL, timer_setting_set, NULL, NULL);

const struct ui_screen timer_screen = {
	.name = "timer",
	.enter = timer_enter,
	.update = timer_update,
	.key = timer_key,
};
