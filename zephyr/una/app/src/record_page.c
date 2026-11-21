/*
 * The recorder's page, and the summary shown when a recording is finished.
 * SELECT starts, pauses and resumes; held, it finishes.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "clock.h"
#include "draw.h"
#include "record.h"
#include "ui.h"

#define SMALL (&lv_font_montserrat_12)
#define TEXT  (&lv_font_montserrat_14)
#define LARGE (&lv_font_montserrat_20)
#define HUGE  (&lv_font_montserrat_48)

/* Above this a pace says less than a speed does. */
#define SPEED_FROM_MM_S 3333 /* 12 km/h */
/* Below this there is no pace to speak of. */
#define PACE_FROM_MM_S  400

static lv_obj_t *canvas;
static struct record_state state;

static const char *duration(uint32_t s)
{
	static char text[16];

	if (s >= 3600) {
		snprintf(text, sizeof(text), "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
	} else {
		snprintf(text, sizeof(text), "%02u:%02u", s / 60, s % 60);
	}
	return text;
}

static const char *kilometres(uint32_t m)
{
	static char text[12];

	snprintf(text, sizeof(text), "%u.%02u", m / 1000, m % 1000 / 10);
	return text;
}

/* A pace in min/km, or a speed in km/h when that says more; `unit` is which. */
static const char *pace(uint32_t mm_s, const char **unit)
{
	static char text[12];

	if (mm_s >= SPEED_FROM_MM_S) {
		uint32_t dm_h = mm_s * 36 / 1000; /* tenths of a km/h */

		*unit = "km/h";
		snprintf(text, sizeof(text), "%u.%u", dm_h / 10, dm_h % 10);
	} else if (mm_s >= PACE_FROM_MM_S) {
		uint32_t s_km = 1000000 / mm_s;

		*unit = "/km";
		snprintf(text, sizeof(text), "%u:%02u", s_km / 60, s_km % 60);
	} else {
		*unit = "/km";
		return "--:--";
	}
	return text;
}

static uint32_t average_mm_s(void)
{
	return state.timer_s > 0 ? (uint64_t)state.distance_m * 1000 / state.timer_s : 0;
}

/* A value with its unit after it, the two centred on x. */
static void value(int x, int y, const char *text, const char *unit, const lv_font_t *font,
		  uint32_t rgb)
{
	lv_point_t a, b;

	lv_text_get_size(&a, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	lv_text_get_size(&b, unit, SMALL, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	x -= (a.x + 3 + b.x) / 2;
	draw_text(x, y, text, font, rgb, DRAW_LEFT);
	draw_text(x + a.x + 3, y + (font == LARGE ? 3 : 1), unit, SMALL, DRAW_WHITE, DRAW_LEFT);
}

static void paint_record(void)
{
	bool running = state.phase == RECORD_RUNNING;
	const char *unit;
	const char *text;
	struct tm local;
	char clock[24];

	if (state.phase == RECORD_IDLE) {
		draw_text(0, -92, "RECORD", TEXT, DRAW_DIM, DRAW_CENTRE);
		draw_text(0, -22, "00:00", HUGE, DRAW_GREY, DRAW_CENTRE);
		draw_text(0, 26, state.gnss ? "GPS OFF" : "NO GPS", TEXT, DRAW_DIM, DRAW_CENTRE);
		if (state.timer_s > 0) {
			/* The one before. */
			snprintf(clock, sizeof(clock), "LAST  %s km", kilometres(state.distance_m));
			draw_text(0, 58, clock, TEXT, DRAW_DIM, DRAW_CENTRE);
			draw_text(0, 76, duration(state.timer_s), TEXT, DRAW_DIM, DRAW_CENTRE);
		}
		return;
	}

	/* The time of day, so that the page need not be left for it. */
	if (clock_local(&local)) {
		snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
		draw_text(0, -90, clock, TEXT, DRAW_WHITE, DRAW_CENTRE);
	}
	if (!running) {
		draw_text(0, -68, "PAUSED", TEXT, DRAW_AMBER, DRAW_CENTRE);
	}
	draw_text(0, -32, duration(state.timer_s), HUGE, running ? DRAW_GREEN : DRAW_WHITE,
		  DRAW_CENTRE);

	value(-52, 16, kilometres(state.distance_m), "km", LARGE, DRAW_GREEN);
	if (state.fix) {
		text = pace(state.speed_mm_s, &unit);
		value(52, 16, text, unit, LARGE, DRAW_GREEN);
	} else if (running) {
		/* Being looked for, or lost. */
		draw_text(52, 16, state.gnss ? "GPS ---" : "NO GPS", TEXT, DRAW_AMBER, DRAW_CENTRE);
	}

	draw_text(-52, 40, "AVG", SMALL, DRAW_DIM, DRAW_CENTRE);
	text = pace(average_mm_s(), &unit);
	value(-52, 56, text, unit, TEXT, DRAW_GREEN);
	draw_text(52, 40, "CLIMB", SMALL, DRAW_DIM, DRAW_CENTRE);
	snprintf(clock, sizeof(clock), "%u", state.ascent_m);
	value(52, 56, clock, "m", TEXT, DRAW_GREEN);

	if (!state.saved) {
		draw_text(0, 82, "NOT SAVED", SMALL, DRAW_AMBER, DRAW_CENTRE);
	} else if (!running) {
		draw_text(0, 82, "hold: finish", SMALL, DRAW_DIM, DRAW_CENTRE);
	}
}

static void record_enter(void)
{
	canvas = draw_create(paint_record);
}

static uint32_t record_update(void)
{
	record_get(&state);
	lv_obj_invalidate(canvas);
	/* The recorder asks for a refresh every second it records. */
	return state.phase == RECORD_PAUSED ? 30000 : UINT32_MAX;
}

/* The summary: what was recorded, and what it was. */

static const struct {
	enum fit_sport sport;
	const char *name;
} labels[] = {
	{FIT_WALK, "WALK"},
	{FIT_RUN, "RUN"},
	{FIT_RIDE, "RIDE"},
	{FIT_HIKE, "HIKE"},
};

static int label;
static bool guess_awaited;

static void paint_summary(void)
{
	const char *unit;
	const char *text;
	char climb[12];
	lv_point_t size;

	draw_text(0, -92, state.saved ? "SAVED" : "NOT SAVED", TEXT,
		  state.saved ? DRAW_DIM : DRAW_AMBER, DRAW_CENTRE);
	value(0, -64, kilometres(state.distance_m), "km", LARGE, DRAW_WHITE);
	draw_text(0, -20, duration(state.timer_s), HUGE, DRAW_WHITE, DRAW_CENTRE);
	text = pace(average_mm_s(), &unit);
	value(-48, 24, text, unit, TEXT, DRAW_WHITE);
	snprintf(climb, sizeof(climb), "+%u", state.ascent_m);
	value(48, 24, climb, "m", TEXT, DRAW_WHITE);

	/* What can be set here. */
	lv_text_get_size(&size, labels[label].name, LARGE, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	draw_box(0, 60, size.x + 24, 28, DRAW_CYAN, true);
	draw_text(0, 60, labels[label].name, LARGE, 0x000000, DRAW_CENTRE);
}

static void summary_enter(void)
{
	canvas = draw_create(paint_summary);
	guess_awaited = true;
}

static uint32_t summary_update(void)
{
	record_get(&state);
	if (state.phase != RECORD_IDLE) {
		/* The recorder has not got to finishing it yet: its guess is to come. */
		return 100;
	}
	if (guess_awaited) {
		guess_awaited = false;
		for (int i = 0; i < ARRAY_SIZE(labels); i++) {
			if (labels[i].sport == state.sport) {
				label = i;
			}
		}
	}
	lv_obj_invalidate(canvas);
	return UINT32_MAX;
}

static bool summary_key(enum ui_key key, bool held)
{
	if (held) {
		return key != UI_KEY_BACK;
	}
	if (key == UI_KEY_UP) {
		label = (label + ARRAY_SIZE(labels) - 1) % ARRAY_SIZE(labels);
	} else if (key == UI_KEY_DOWN) {
		label = (label + 1) % ARRAY_SIZE(labels);
	} else if (key == UI_KEY_SELECT) {
		ui_pop();
	} else {
		return false;
	}
	return true;
}

/* However the summary is left, the label is what it showed. */
static void summary_leave(void)
{
	record_label(labels[label].sport);
}

static const struct ui_screen summary_screen = {
	.name = "record summary",
	.enter = summary_enter,
	.update = summary_update,
	.key = summary_key,
	.leave = summary_leave,
};

static bool record_key(enum ui_key key, bool held)
{
	if (key != UI_KEY_SELECT) {
		return false;
	}
	record_get(&state);
	if (held) {
		if (state.phase != RECORD_IDLE) {
			record_finish();
			ui_push(&summary_screen);
		}
	} else if (state.phase == RECORD_IDLE) {
		record_start();
	} else if (state.phase == RECORD_RUNNING) {
		record_pause();
	} else {
		record_resume();
	}
	return true;
}

const struct ui_screen record_screen = {
	.name = "record",
	.enter = record_enter,
	.update = record_update,
	.key = record_key,
};
