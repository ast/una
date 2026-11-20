/*
 * Settings, and the two things kept with them: power off and "about".
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include <zephyr/app_version.h>

#include "alert.h"
#include "power.h"
#include "ui.h"

/* About */

static void about_enter(void)
{
	lv_label_set_text(ui_label(&lv_font_montserrat_14, 0xaaaaaa, -92), "About");
	lv_label_set_text(ui_label(&lv_font_montserrat_20, 0xffffff, -10),
			  "UNA Watch\nZephyr " APP_VERSION_STRING);
	lv_label_set_text(ui_label(&lv_font_montserrat_14, 0xaaaaaa, 50), "built " __DATE__);
}

static uint32_t about_update(void)
{
	return UINT32_MAX;
}

static void about_key(enum ui_key key, bool held)
{
	ARG_UNUSED(key);
	ARG_UNUSED(held);

	ui_pop();
}

static const struct ui_screen about_screen = {
	.name = "about",
	.enter = about_enter,
	.update = about_update,
	.key = about_key,
};

/* The list */

enum {
	ROW_BACKLIGHT,
	ROW_SOUND,
	ROW_VIBRATION,
	ROW_POWER_OFF,
	ROW_ABOUT,
	ROW_COUNT,
};

static struct ui_list list;

static const char *row_text(int index)
{
	static char text[24];

	switch (index) {
	case ROW_BACKLIGHT:
		if (ui_backlight_s() == 0) {
			return "Backlight off";
		}
		snprintf(text, sizeof(text), "Backlight %d s", ui_backlight_s());
		return text;
	case ROW_SOUND:
		return alert_sound() ? "Sound on" : "Sound off";
	case ROW_VIBRATION:
		return alert_vibration() ? "Vibration on" : "Vibration off";
	case ROW_POWER_OFF:
		return "Power off";
	default:
		return "About";
	}
}

static void do_power_off(void)
{
	/* Does not return on battery; on USB power the PMIC starts again. */
	power_off();
}

static void settings_enter(void)
{
	ui_list_create(&list, "Settings", ROW_COUNT);
}

static uint32_t settings_update(void)
{
	ui_list_show(&list, row_text);
	return UINT32_MAX;
}

static void settings_key(enum ui_key key, bool held)
{
	static const int backlight_choices[] = {3, 5, 10, 20, 0};

	if (held || ui_list_key(&list, key)) {
		return;
	}
	if (key == UI_KEY_BACK) {
		ui_pop();
		return;
	}
	switch (list.selected) {
	case ROW_BACKLIGHT:
		for (int i = 0; i < ARRAY_SIZE(backlight_choices); i++) {
			if (backlight_choices[i] == ui_backlight_s()) {
				ui_set_backlight_s(
					backlight_choices[(i + 1) % ARRAY_SIZE(backlight_choices)]);
				return;
			}
		}
		ui_set_backlight_s(backlight_choices[1]);
		break;
	case ROW_SOUND:
		alert_set_sound(!alert_sound());
		alert_blip();
		break;
	case ROW_VIBRATION:
		alert_set_vibration(!alert_vibration());
		alert_blip();
		break;
	case ROW_POWER_OFF:
		ui_confirm("Power off?", do_power_off);
		break;
	case ROW_ABOUT:
		ui_push(&about_screen);
		break;
	}
}

const struct ui_screen settings_screen = {
	.name = "settings",
	.enter = settings_enter,
	.update = settings_update,
	.key = settings_key,
};
