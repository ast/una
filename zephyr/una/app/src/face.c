/*
 * Watch face: the digital one, or one of the analogue dials (src/dial.c),
 * and the screen that chooses between them.
 */

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>

#include "alarm.h"
#include "clock.h"
#include "dial.h"
#include "face.h"
#include "power.h"
#include "ui.h"

/* 0 is the digital face; the dials follow. */
#define FACE_COUNT (1 + DIAL_COUNT)

static int32_t face;
static uint8_t light = 1;

const char *face_name(void)
{
	return face == 0 ? "Digital" : dial_name(face - 1);
}

bool face_light(void)
{
	return light;
}

void face_set_light(bool on)
{
	light = on;
	settings_save_one("face/light", &light, sizeof(light));
}

static int face_setting_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "style", NULL) && len == sizeof(face)) {
		if (read_cb(cb_arg, &face, sizeof(face)) < 0) {
			return -EIO;
		}
		face = CLAMP(face, 0, FACE_COUNT - 1);
		return 0;
	}
	if (settings_name_steq(name, "light", NULL) && len == sizeof(light)) {
		return read_cb(cb_arg, &light, sizeof(light)) < 0 ? -EIO : 0;
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(face, "face", NULL, face_setting_set, NULL, NULL);

static lv_obj_t *time_label;
static lv_obj_t *date_label;
static lv_obj_t *battery_label;
static lv_obj_t *alarm_label;

static void face_enter(void)
{
	if (face != 0) {
		dial_create();
		return;
	}
	date_label = ui_label(&lv_font_montserrat_20, 0xaaaaaa, -52);
	time_label = ui_label(&lv_font_montserrat_48, 0xffffff, 0);
	battery_label = ui_label(&lv_font_montserrat_20, 0xffffff, 54);
	alarm_label = ui_label(&lv_font_montserrat_14, 0xaaaaaa, 86);
}

static void digital_update(const struct tm *now, const struct power_status *st)
{
	static const char *const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
					     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	int hour, minute;

	if (now != NULL) {
		lv_label_set_text_fmt(time_label, "%02d:%02d", now->tm_hour, now->tm_min);
		lv_label_set_text_fmt(date_label, "%s %d %s", days[now->tm_wday], now->tm_mday,
				      months[now->tm_mon]);
	} else {
		lv_label_set_text(time_label, "--:--");
		lv_label_set_text(date_label, "clock not set");
	}

	if (alarm_get(&hour, &minute)) {
		lv_label_set_text_fmt(alarm_label, LV_SYMBOL_BELL " %02d:%02d", hour, minute);
	} else {
		lv_label_set_text(alarm_label, "");
	}

	if (st->millivolts == 0) {
		lv_label_set_text(battery_label, "");
		return;
	}
	lv_label_set_text_fmt(battery_label, "%d%%%s", st->percent, st->usb_power ? " +" : "");
	/* The panel's four levels per colour: these three are exact. */
	lv_obj_set_style_text_color(battery_label,
				    lv_color_hex(st->percent <= 10   ? 0xff0000
						 : st->percent <= 25 ? 0xffff00
								     : 0x00ff00),
				    0);
}

static void dial_update(const struct tm *now, const struct power_status *st)
{
	struct dial_state state = {
		.style = face - 1,
		.light = light,
		.battery = st->percent,
	};

	/* A clock that is not set shows twelve and no date. */
	if (now != NULL) {
		state.hour = now->tm_hour;
		state.minute = now->tm_min;
		state.day = now->tm_mday;
	}
	state.alarm_on = alarm_get(&state.alarm_hour, &state.alarm_minute);
	dial_set(&state);
}

/* Due again when the minute changes. */
static uint32_t face_update(void)
{
	struct power_status st;
	struct tm now;
	bool set = clock_local(&now);

	power_last(&st);
	if (face == 0) {
		digital_update(set ? &now : NULL, &st);
	} else {
		dial_update(set ? &now : NULL, &st);
	}
	if (st.millivolts == 0) {
		/* No battery reading yet: the first comes a few seconds after boot. */
		return 2000;
	}
	return set ? (60 - now.tm_sec) * 1000 : 60000;
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

/* The chooser: the face itself, with its name; UP and DOWN go through them. */

static lv_obj_t *name_label;

static void chooser_enter(void)
{
	face_enter();
	name_label = ui_label(&lv_font_montserrat_14, 0x000000, 96);
	lv_obj_set_style_bg_color(name_label, lv_color_hex(0xffff00), 0);
	lv_obj_set_style_bg_opa(name_label, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(name_label, 9, 0);
	lv_obj_set_style_pad_hor(name_label, 8, 0);
	lv_obj_set_style_pad_ver(name_label, 1, 0);
}

static uint32_t chooser_update(void)
{
	lv_label_set_text(name_label, face_name());
	return face_update();
}

static void chooser_key(enum ui_key key, bool held)
{
	if (key == UI_KEY_UP || key == UI_KEY_DOWN) {
		face = (face + FACE_COUNT + (key == UI_KEY_DOWN ? 1 : -1)) % FACE_COUNT;
		settings_save_one("face/style", &face, sizeof(face));
		/* Another face is other widgets. */
		lv_obj_clean(lv_screen_active());
		chooser_enter();
	} else if (!held) {
		ui_pop();
	}
}

const struct ui_screen face_chooser_screen = {
	.name = "face chooser",
	.enter = chooser_enter,
	.update = chooser_update,
	.key = chooser_key,
};

/* face [<number> [<white 0|1>]] */
static int cmd_face(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		face = CLAMP(atoi(argv[1]), 0, FACE_COUNT - 1);
		settings_save_one("face/style", &face, sizeof(face));
	}
	if (argc > 2) {
		face_set_light(atoi(argv[2]) != 0);
	}
	ui_reload();
	shell_print(sh, "face %d of %d: %s, dial %s", (int)face, FACE_COUNT, face_name(),
		    light ? "white" : "black");
	return 0;
}
SHELL_CMD_ARG_REGISTER(face, NULL, "Show or set the watch face: face [<number> [<white 0|1>]]",
		       cmd_face, 1, 2);
