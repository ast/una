/*
 * Stopwatch. Keeps running when another screen is shown.
 */

#include <zephyr/kernel.h>

#include "ui.h"

static bool running;
static int64_t started_at;
static int64_t banked_ms;

static lv_obj_t *time_label;
static lv_obj_t *fraction_label;
static lv_obj_t *hint_label;

static void stopwatch_enter(void)
{
	ui_title("STOPWATCH");
	time_label = ui_label(&lv_font_montserrat_48, 0xffffff, -14);
	fraction_label = ui_label(&lv_font_montserrat_20, 0xffffff, 28);
	hint_label = ui_label(&lv_font_montserrat_14, 0xaaaaaa, 72);
}

/* Whole seconds while running: a frame is too costly for more. */
static uint32_t stopwatch_update(void)
{
	int64_t ms = banked_ms + (running ? k_uptime_get() - started_at : 0);
	int s = ms / 1000;

	if (s >= 3600) {
		lv_label_set_text_fmt(time_label, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	} else {
		lv_label_set_text_fmt(time_label, "%02d:%02d", s / 60, s % 60);
	}
	if (running) {
		lv_label_set_text(fraction_label, "");
		lv_label_set_text(hint_label, "");
		lv_obj_set_style_text_color(time_label, lv_color_hex(0x00ff00), 0);
		return 1000 - ms % 1000;
	}
	lv_label_set_text_fmt(fraction_label, ".%02d", (int)(ms % 1000 / 10));
	lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
	lv_label_set_text(hint_label, ms == 0 ? "" : "hold: reset");
	return UINT32_MAX;
}

/* SELECT starts and stops; held, it resets a stopped watch. */
static bool stopwatch_key(enum ui_key key, bool held)
{
	if (key != UI_KEY_SELECT) {
		return false;
	}
	if (held) {
		if (!running) {
			banked_ms = 0;
		}
	} else {
		if (running) {
			banked_ms += k_uptime_get() - started_at;
		} else {
			started_at = k_uptime_get();
		}
		running = !running;
	}
	return true;
}

const struct ui_screen stopwatch_screen = {
	.name = "stopwatch",
	.enter = stopwatch_enter,
	.update = stopwatch_update,
	.key = stopwatch_key,
};
