/*
 * Settings, and power off. The page shows the list; SELECT goes in, and
 * then changes the row it is on.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alert.h"
#include "bluetooth.h"
#include "motion.h"
#include "power.h"
#include "ui.h"

enum {
	ROW_BACKLIGHT,
	ROW_WRIST,
	ROW_SOUND,
	ROW_VIBRATION,
	ROW_BLUETOOTH,
	ROW_FORGET,
	ROW_POWER_OFF,
	ROW_COUNT,
};

static struct ui_list list = {.quiet = true};

static const char *row_text(int index)
{
	static char text[24];
	struct bluetooth_status radio;

	switch (index) {
	case ROW_BACKLIGHT:
		if (ui_backlight_s() == 0) {
			return "Backlight off";
		}
		snprintf(text, sizeof(text), "Backlight %d s", ui_backlight_s());
		return text;
	case ROW_WRIST:
		return motion_wrist_light() ? "Wrist light on" : "Wrist light off";
	case ROW_SOUND:
		return alert_sound() ? "Sound on" : "Sound off";
	case ROW_VIBRATION:
		return alert_vibration() ? "Vibration on" : "Vibration off";
	case ROW_BLUETOOTH:
		return bluetooth_is_set() ? "Bluetooth on" : "Bluetooth off";
	case ROW_FORGET:
		/* Held, as it cannot be taken back. */
		bluetooth_status(&radio);
		snprintf(text, sizeof(text), "Hold: unpair (%u)", radio.bonds);
		return text;
	default:
		/* Asked for by holding, in place of a question afterwards. */
		return "Hold: power off";
	}
}

static void settings_enter(void)
{
	list.quiet = true;
	ui_list_create(&list, "SETTINGS", ROW_COUNT);
}

static uint32_t settings_update(void)
{
	ui_list_show(&list, row_text);
	return UINT32_MAX;
}

static void next_backlight(void)
{
	static const int choices[] = {3, 5, 10, 20, 0};

	for (int i = 0; i < ARRAY_SIZE(choices); i++) {
		if (choices[i] == ui_backlight_s()) {
			ui_set_backlight_s(choices[(i + 1) % ARRAY_SIZE(choices)]);
			return;
		}
	}
	ui_set_backlight_s(choices[1]);
}

static bool settings_key(enum ui_key key, bool held)
{
	if (list.quiet) {
		if (key != UI_KEY_SELECT) {
			return false;
		}
		list.quiet = false;
		return true;
	}
	if (key == UI_KEY_BACK) {
		list.quiet = true;
		return true;
	}
	if (held) {
		if (key == UI_KEY_SELECT && list.selected == ROW_POWER_OFF) {
			/* Does not return on battery; on USB power the PMIC starts again. */
			power_off();
		}
		if (key == UI_KEY_SELECT && list.selected == ROW_FORGET) {
			bluetooth_forget();
			alert_blip();
		}
		return true;
	}
	if (ui_list_key(&list, key)) {
		return true;
	}
	switch (list.selected) {
	case ROW_BACKLIGHT:
		next_backlight();
		break;
	case ROW_WRIST:
		motion_set_wrist_light(!motion_wrist_light());
		break;
	case ROW_SOUND:
		alert_set_sound(!alert_sound());
		alert_blip();
		break;
	case ROW_VIBRATION:
		alert_set_vibration(!alert_vibration());
		alert_blip();
		break;
	case ROW_BLUETOOTH:
		bluetooth_set(!bluetooth_is_set());
		break;
	}
	return true;
}

const struct ui_screen settings_screen = {
	.name = "settings",
	.enter = settings_enter,
	.update = settings_update,
	.key = settings_key,
};
