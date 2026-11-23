/*
 * The pulse page: the sensor is on while it is looked at, and the pulse is
 * there some ten seconds after the watch has found skin.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "draw.h"
#include "hr.h"
#include "ui.h"

#define SMALL (&lv_font_montserrat_12)
#define TEXT  (&lv_font_montserrat_20)
#define LARGE (&lv_font_montserrat_48)

static lv_obj_t *canvas;
static struct hr_status status;
static int64_t entered_ms;

static void paint(void)
{
	char text[16];

	draw_text(0, -62, "PULSE", SMALL, DRAW_DIM, DRAW_CENTRE);
	if (!status.on) {
		/* It is on at once, if it is there. */
		if (k_uptime_get() - entered_ms > 1000) {
			draw_text(0, 0, "NO SENSOR", TEXT, DRAW_AMBER, DRAW_CENTRE);
		}
		return;
	}
	if (!status.worn) {
		draw_text(0, 0, "NOT WORN", TEXT, DRAW_GREY, DRAW_CENTRE);
		return;
	}
	if (status.bpm == 0) {
		draw_text(0, -14, "--", LARGE, DRAW_GREY, DRAW_CENTRE);
		draw_text(0, 24, "HOLD STILL", SMALL, DRAW_DIM, DRAW_CENTRE);
		return;
	}
	snprintf(text, sizeof(text), "%u", status.bpm);
	draw_text(0, -14, text, LARGE, DRAW_GREEN, DRAW_CENTRE);
	draw_text(0, 24, "A MINUTE", SMALL, DRAW_DIM, DRAW_CENTRE);
}

static void hr_enter(void)
{
	hr_want(HR_FOR_PAGE, true);
	entered_ms = k_uptime_get();
	canvas = draw_create(paint);
}

static uint32_t hr_update(void)
{
	hr_status(&status);
	lv_obj_invalidate(canvas);
	return 1000;
}

/* Nothing to do here; the buttons do what they do everywhere. */
static bool hr_key(enum ui_key key, bool held)
{
	ARG_UNUSED(key);
	ARG_UNUSED(held);

	return false;
}

/* It is not left on by having been looked at. */
static void hr_leave(void)
{
	hr_want(HR_FOR_PAGE, false);
}

const struct ui_screen hr_screen = {
	.name = "pulse",
	.enter = hr_enter,
	.update = hr_update,
	.key = hr_key,
	.leave = hr_leave,
};
