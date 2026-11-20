/*
 * The menu behind the watch face's SELECT button.
 */

#include <zephyr/sys/util.h>

#include "ui.h"

static const struct {
	const char *name;
	const struct ui_screen *screen;
} entries[] = {
	{"Stopwatch", &stopwatch_screen},
	{"Timer", &timer_screen},
	{"Alarm", &alarm_screen},
	{"Battery", &status_screen},
	{"Settings", &settings_screen},
};

static struct ui_list list;

static const char *entry_text(int index)
{
	return entries[index].name;
}

static void menu_enter(void)
{
	ui_list_create(&list, "Menu", ARRAY_SIZE(entries));
}

static uint32_t menu_update(void)
{
	ui_list_show(&list, entry_text);
	return UINT32_MAX;
}

static void menu_key(enum ui_key key, bool held)
{
	if (held || ui_list_key(&list, key)) {
		return;
	}
	if (key == UI_KEY_SELECT) {
		ui_push(entries[list.selected].screen);
	} else {
		ui_pop();
	}
}

const struct ui_screen menu_screen = {
	.name = "menu",
	.enter = menu_enter,
	.update = menu_update,
	.key = menu_key,
};
