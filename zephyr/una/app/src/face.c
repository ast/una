/*
 * Watch face: time, date, battery.
 */

#include <zephyr/kernel.h>

#include "alarm.h"
#include "clock.h"
#include "power.h"
#include "ui.h"

static lv_obj_t *time_label;
static lv_obj_t *date_label;
static lv_obj_t *battery_label;
static lv_obj_t *alarm_label;

static void face_enter(void)
{
	date_label = ui_label(&lv_font_montserrat_20, 0xaaaaaa, -52);
	time_label = ui_label(&lv_font_montserrat_48, 0xffffff, 0);
	battery_label = ui_label(&lv_font_montserrat_20, 0xffffff, 54);
	alarm_label = ui_label(&lv_font_montserrat_14, 0xaaaaaa, 86);
}

/* Due again when the minute changes. */
static uint32_t face_update(void)
{
	static const char *const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
					     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	struct power_status st;
	struct tm now;
	int left = 60;
	int hour, minute;

	if (clock_local(&now)) {
		lv_label_set_text_fmt(time_label, "%02d:%02d", now.tm_hour, now.tm_min);
		lv_label_set_text_fmt(date_label, "%s %d %s", days[now.tm_wday], now.tm_mday,
				      months[now.tm_mon]);
		left = 60 - now.tm_sec;
	} else {
		lv_label_set_text(time_label, "--:--");
		lv_label_set_text(date_label, "clock not set");
	}

	if (alarm_get(&hour, &minute)) {
		lv_label_set_text_fmt(alarm_label, LV_SYMBOL_BELL " %02d:%02d", hour, minute);
	} else {
		lv_label_set_text(alarm_label, "");
	}

	power_last(&st);
	if (st.millivolts == 0) {
		/* No reading yet: the first comes a few seconds after boot. */
		lv_label_set_text(battery_label, "");
		return 2000;
	}
	lv_label_set_text_fmt(battery_label, "%d%%%s", st.percent, st.usb_power ? " +" : "");
	/* The panel's four levels per colour: these three are exact. */
	lv_obj_set_style_text_color(battery_label,
				    lv_color_hex(st.percent <= 10   ? 0xff0000
						 : st.percent <= 25 ? 0xffff00
								    : 0x00ff00),
				    0);
	return left * 1000;
}

static void face_key(enum ui_key key, bool held)
{
	if (held) {
		return;
	}
	if (key == UI_KEY_SELECT) {
		ui_push(&menu_screen);
	} else if (key == UI_KEY_UP || key == UI_KEY_DOWN) {
		ui_push(&status_screen);
	}
}

const struct ui_screen face_screen = {
	.name = "face",
	.enter = face_enter,
	.update = face_update,
	.key = face_key,
};
