/*
 * The compass page: where twelve o'clock points, and a rose that stays put
 * while the watch is turned under it. Holding SELECT calibrates.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "compass.h"
#include "draw.h"
#include "ui.h"

#define SMALL (&lv_font_montserrat_12)
#define TEXT  (&lv_font_montserrat_20)
#define LARGE (&lv_font_montserrat_48)

static lv_obj_t *canvas;
static struct compass_status status;
static struct compass_reading reading;
static bool have_reading;
static int64_t entered_ms;

/* Along a radius of the rose, which is turned back by the heading. */
static int rose(int deg)
{
	return ((deg - reading.heading_deg) % 360 + 360) % 360;
}

static void paint(void)
{
	static const char *const points[] = {"N",  "NNE", "NE", "ENE", "E",  "ESE", "SE", "SSE",
					     "S",  "SSW", "SW", "WSW", "W",  "WNW", "NW", "NNW"};
	static const char *const quarters[] = {"N", "E", "S", "W"};
	char text[16];

	if (!status.on) {
		/* It is on within a fifth of a second, if it is there. */
		if (k_uptime_get() - entered_ms > 1000) {
			draw_text(0, 0, "NO SENSOR", TEXT, DRAW_AMBER, DRAW_CENTRE);
		}
		return;
	}

	/* The mark at twelve, which is what the heading is of. */
	draw_radial(0, 100, 112, 3, DRAW_WHITE, false);

	if (have_reading) {
		/* The rose: it lies as the world does, so north is back by the heading. */
		for (int deg = 0; deg < 360; deg += 30) {
			bool quarter = deg % 90 == 0;

			draw_radial(rose(deg), quarter ? 84 : 88, 96, quarter ? 3 : 1,
				    deg == 0 ? DRAW_RED : DRAW_DIM, false);
		}
		for (int i = 0; i < 4; i++) {
			int deg = rose(i * 90);

			draw_text((lv_trigo_sin(deg) * 70 + 16384) >> 15,
				  -((lv_trigo_cos(deg) * 70 + 16384) >> 15), quarters[i], TEXT,
				  i == 0 ? DRAW_RED : DRAW_DIM, DRAW_CENTRE);
		}
	}

	if (status.calibrating) {
		snprintf(text, sizeof(text), "%d%%", status.progress);
		draw_text(0, -14, text, LARGE, DRAW_CYAN, DRAW_CENTRE);
		draw_text(0, 22, "TURN IT", SMALL, DRAW_WHITE, DRAW_CENTRE);
		draw_text(0, 36, "EVERY WAY", SMALL, DRAW_WHITE, DRAW_CENTRE);
		return;
	}
	if (!have_reading) {
		return;
	}
	snprintf(text, sizeof(text), "%d°", reading.heading_deg);
	draw_text(0, -14, text, LARGE, status.calibrated ? DRAW_WHITE : DRAW_AMBER, DRAW_CENTRE);
	if (status.calibrated) {
		/* Sixteen points, each 22.5 degrees wide about its own direction. */
		draw_text(0, 24, points[(reading.heading_deg * 2 + 22) / 45 % 16], TEXT, DRAW_DIM,
			  DRAW_CENTRE);
	} else {
		draw_text(0, 22, "HOLD TOP RIGHT", SMALL, DRAW_AMBER, DRAW_CENTRE);
		draw_text(0, 36, "TO CALIBRATE", SMALL, DRAW_AMBER, DRAW_CENTRE);
	}
}

static void compass_enter(void)
{
	compass_want(COMPASS_FOR_PAGE, true);
	entered_ms = k_uptime_get();
	canvas = draw_create(paint);
}

static uint32_t compass_update(void)
{
	compass_status(&status);
	have_reading = compass_read(&reading);
	lv_obj_invalidate(canvas);
	return status.on ? 200 : 1000;
}

static bool compass_key(enum ui_key key, bool held)
{
	if (key != UI_KEY_SELECT) {
		return false;
	}
	if (held || status.calibrating) {
		compass_calibrate(!status.calibrating);
	}
	return true;
}

/* It is not left on by having been looked at. */
static void compass_leave(void)
{
	compass_calibrate(false);
	compass_want(COMPASS_FOR_PAGE, false);
}

const struct ui_screen compass_screen = {
	.name = "compass",
	.enter = compass_enter,
	.update = compass_update,
	.key = compass_key,
	.leave = compass_leave,
};
