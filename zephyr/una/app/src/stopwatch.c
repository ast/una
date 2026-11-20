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
	lv_label_set_text(ui_label(&lv_font_montserrat_14, 0xaaaaaa, -92), "Stopwatch");
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
		lv_label_set_text(hint_label, "top right: stop");
		return 1000 - ms % 1000;
	}
	lv_label_set_text_fmt(fraction_label, ".%02d", (int)(ms % 1000 / 10));
	lv_label_set_text(hint_label, ms == 0 ? "top right: start"
					      : "top right: start\nbottom left: reset");
	return UINT32_MAX;
}

static void stopwatch_key(enum ui_key key, bool held)
{
	if (held) {
		return;
	}
	if (key == UI_KEY_SELECT) {
		if (running) {
			banked_ms += k_uptime_get() - started_at;
		} else {
			started_at = k_uptime_get();
		}
		running = !running;
	} else if (key == UI_KEY_DOWN && !running) {
		banked_ms = 0;
	} else if (key == UI_KEY_BACK) {
		ui_pop();
	}
}

const struct ui_screen stopwatch_screen = {
	.name = "stopwatch",
	.enter = stopwatch_enter,
	.update = stopwatch_update,
	.key = stopwatch_key,
};
