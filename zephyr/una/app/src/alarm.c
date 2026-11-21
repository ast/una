/*
 * One daily alarm.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "alarm.h"
#include "alert.h"
#include "clock.h"
#include "ui.h"

#define RING_S 60

static struct {
	uint8_t hour;
	uint8_t minute;
	uint8_t on;
} alarm = {.hour = 7};

bool alarm_get(int *hour, int *minute)
{
	*hour = alarm.hour;
	*minute = alarm.minute;
	return alarm.on;
}

static void check(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(check_work, check);

/*
 * Looks at the clock at the alarm's minute, and at least once an hour on the
 * way there, so that a wait of many hours does not drift off the RTC.
 */
static void schedule(void)
{
	struct tm now;
	int wait_s = 60;

	if (!alarm.on) {
		k_work_cancel_delayable(&check_work);
		return;
	}
	if (clock_local(&now)) {
		wait_s = (alarm.hour * 60 + alarm.minute - now.tm_hour * 60 - now.tm_min) * 60 -
			 now.tm_sec;
		if (wait_s <= 0) {
			wait_s += 24 * 3600;
		}
	}
	k_work_reschedule(&check_work, K_SECONDS(MIN(wait_s, 3600)));
}

static void check(struct k_work *work)
{
	struct tm now;

	ARG_UNUSED(work);

	if (alarm.on && clock_local(&now) && now.tm_hour == alarm.hour &&
	    now.tm_min == alarm.minute) {
		alert_ring("Alarm", RING_S);
	}
	schedule();
}

void alarm_clock_changed(void)
{
	schedule();
}

static int alarm_setting_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "daily", NULL) && len == sizeof(alarm)) {
		return read_cb(cb_arg, &alarm, sizeof(alarm)) < 0 ? -EIO : 0;
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(alarm, "alarm", NULL, alarm_setting_set, NULL, NULL);

/* After clock_init, which loads the settings. */
static int alarm_init(void)
{
	schedule();
	return 0;
}
SYS_INIT(alarm_init, APPLICATION, 97);

/*
 * The page: SELECT turns the alarm on and off; held, it goes in to set the
 * hour, then the minute.
 */

enum field {
	FIELD_NONE, /* not inside */
	FIELD_HOUR,
	FIELD_MINUTE,
};

static enum field field;
static lv_obj_t *hour_label;
static lv_obj_t *colon_label;
static lv_obj_t *minute_label;
static lv_obj_t *on_label;
static lv_obj_t *hint_label;

static void alarm_enter(void)
{
	ui_title("ALARM");
	hour_label = ui_label(&lv_font_montserrat_48, 0xffffff, -14);
	lv_obj_align(hour_label, LV_ALIGN_CENTER, -40, -14);
	colon_label = ui_label(&lv_font_montserrat_48, 0xffffff, -18);
	lv_label_set_text(colon_label, ":");
	minute_label = ui_label(&lv_font_montserrat_48, 0xffffff, -14);
	lv_obj_align(minute_label, LV_ALIGN_CENTER, 40, -14);
	on_label = ui_label(&lv_font_montserrat_20, 0xffffff, 32);
	hint_label = ui_label(&lv_font_montserrat_14, 0xaaaaaa, 72);
	field = FIELD_NONE;
}

static uint32_t alarm_update(void)
{
	/* An alarm that is off is grey; the field being set is cyan. */
	const lv_color_t plain = lv_color_hex(alarm.on ? 0xffffff : 0x555555);
	const lv_color_t chosen = lv_color_hex(0x00ffff);

	lv_label_set_text_fmt(hour_label, "%02d", alarm.hour);
	lv_label_set_text_fmt(minute_label, "%02d", alarm.minute);
	lv_label_set_text(on_label, alarm.on ? "ON" : "OFF");
	lv_obj_set_style_text_color(on_label, lv_color_hex(alarm.on ? 0x00ff00 : 0x555555), 0);
	lv_obj_set_style_text_color(colon_label, plain, 0);
	lv_obj_set_style_text_color(hour_label, field == FIELD_HOUR ? chosen : plain, 0);
	lv_obj_set_style_text_color(minute_label, field == FIELD_MINUTE ? chosen : plain, 0);
	lv_label_set_text(hint_label, field == FIELD_NONE ? "hold: set" : "");
	return UINT32_MAX;
}

static bool alarm_key(enum ui_key key, bool held)
{
	int dir = key == UI_KEY_UP ? 1 : -1;

	if (field == FIELD_NONE) {
		if (key != UI_KEY_SELECT) {
			return false;
		}
		if (held) {
			field = FIELD_HOUR;
			return true;
		}
		alarm.on = !alarm.on;
	} else if (key == UI_KEY_SELECT || key == UI_KEY_BACK) {
		if (!held) {
			field = key == UI_KEY_SELECT && field == FIELD_HOUR ? FIELD_MINUTE
									     : FIELD_NONE;
		}
		return true;
	} else if (field == FIELD_HOUR) {
		alarm.hour = (alarm.hour + 24 + dir) % 24;
	} else if (held) {
		/* Fives, to the next multiple; held: single minutes. */
		alarm.minute = (alarm.minute + 60 + dir) % 60;
	} else if (dir > 0) {
		alarm.minute = (alarm.minute / 5 * 5 + 5) % 60;
	} else {
		alarm.minute = ((alarm.minute + 4) / 5 * 5 + 55) % 60;
	}
	settings_save_one("alarm/daily", &alarm, sizeof(alarm));
	schedule();
	return true;
}

const struct ui_screen alarm_screen = {
	.name = "alarm",
	.enter = alarm_enter,
	.update = alarm_update,
	.key = alarm_key,
};
