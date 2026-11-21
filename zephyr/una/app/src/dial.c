/*
 * Analogue dials, drawn directly: one object whose draw event paints the
 * dial, what is on it, and the hands. Hour and minute hand only, so a dial
 * is redrawn once a minute.
 *
 * The panel has four levels per colour: every colour here is one it has.
 */

#include <stdio.h>

#include <zephyr/sys/util.h>

#include "dial.h"
#include "draw.h"

#define RED    0xff0000
#define YELLOW 0xffaa00

static struct dial_state state;
static lv_obj_t *dial;

/* Set at the start of a draw. */
static uint32_t fg, bg, dim;

const char *dial_name(enum dial_style style)
{
	static const char *const names[DIAL_COUNT] = {"Bill", "Rams", "Station", "Tangente"};

	return names[style];
}

static void disc(int radius, uint32_t rgb)
{
	draw_arc(0, 0, radius, radius, 0, 360, rgb);
}

static void ticks(int every_deg, int skip_deg, int r1, int r2, int width, uint32_t rgb)
{
	for (int deg = 0; deg < 360; deg += every_deg) {
		if (skip_deg == 0 || deg % skip_deg != 0) {
			draw_radial(deg, r1, r2, width, rgb, false);
		}
	}
}

static void numeral(int hour, int r)
{
	char str[3];

	snprintf(str, sizeof(str), "%d", hour);
	draw_text((lv_trigo_sin(hour * 30) * r + 16384) >> 15,
		  -((lv_trigo_cos(hour * 30) * r + 16384) >> 15), str, &lv_font_montserrat_20, fg,
		  DRAW_CENTRE);
}

static void hands(int hour_len, int hour_width, int minute_len, int minute_width, int tail,
		  uint32_t rgb, bool round)
{
	int hour_deg = state.hour % 12 * 30 + state.minute / 2;

	draw_radial(hour_deg, -tail, hour_len, hour_width, rgb, round);
	draw_radial(state.minute * 6, -tail, minute_len, minute_width, rgb, round);
}

/* What every dial carries, each in its own manner. */

static uint32_t battery_colour(uint32_t normal)
{
	return state.battery <= 10 ? RED : normal;
}

static void battery_text(int y)
{
	char str[6];

	snprintf(str, sizeof(str), "%d%%", state.battery);
	draw_text(0, y, str, &lv_font_montserrat_14, battery_colour(dim), DRAW_CENTRE);
}

static void date(int x, bool window)
{
	char str[4];

	if (state.day == 0) {
		return;
	}
	snprintf(str, sizeof(str), "%d", state.day);
	if (window) {
		draw_box(x, 0, 27, 21, dim, false);
	}
	draw_text(x, 0, str, &lv_font_montserrat_14, window ? fg : dim, DRAW_CENTRE);
}

static void alarm(int y)
{
	char str[16];

	if (state.alarm_on) {
		snprintf(str, sizeof(str), LV_SYMBOL_BELL " %02d:%02d", state.alarm_hour,
			 state.alarm_minute);
		draw_text(0, y, str, &lv_font_montserrat_14, dim, DRAW_CENTRE);
	}
}

/* The dials */

static void draw_bill(void)
{
	/* The battery as a short arc under the centre, filled from the left. */
	const int span = 80;
	int level = span * state.battery / 100;

	ticks(6, 30, 106, 114, 1, fg);
	ticks(30, 0, 98, 114, 2, fg);
	date(62, false);
	alarm(-50);
	draw_arc(0, 0, 50, 2, 90 - span / 2, 90 + span / 2, dim);
	draw_arc(0, 0, 51, 4, 90 + span / 2 - level, 90 + span / 2, battery_colour(fg));
	hands(62, 4, 100, 3, 0, fg, true);
	disc(4, fg);
}

static void draw_rams(void)
{
	ticks(6, 30, 110, 116, 1, fg);
	ticks(30, 0, 106, 116, 3, fg);
	for (int hour = 1; hour <= 12; hour++) {
		numeral(hour, 90);
	}
	date(50, true);
	alarm(-46);
	battery_text(46);
	hands(56, 6, 86, 4, 12, fg, false);
	disc(7, fg);
	disc(3, YELLOW);
}

static void draw_station(void)
{
	ticks(6, 30, 104, 114, 2, fg);
	ticks(30, 0, 88, 114, 7, fg);
	date(56, false);
	alarm(-46);
	battery_text(46);
	hands(66, 11, 100, 8, 18, fg, false);
	disc(5, RED);
}

static void draw_tangente(void)
{
	/* Where the original has its seconds: the battery, as a gauge. */
	const int sub_y = 56, sub_r = 25, sweep = 270;
	uint32_t blue = state.light ? 0x0000aa : 0x55aaff;
	char str[4];

	ticks(6, 0, 109, 114, 1, fg);
	for (int hour = 1; hour <= 12; hour++) {
		if (hour % 2 == 1) {
			draw_radial(hour * 30, 80, 98, 2, fg, false);
		} else if (hour != 6) {
			numeral(hour, 89);
		}
	}
	draw_arc(0, sub_y, sub_r, 1, 0, 360, dim);
	draw_arc(0, sub_y, sub_r - 3, 3, 135, 135 + sweep * state.battery / 100, battery_colour(fg));
	snprintf(str, sizeof(str), "%d", state.battery);
	draw_text(0, sub_y, str, &lv_font_montserrat_14, battery_colour(dim), DRAW_CENTRE);
	date(54, false);
	alarm(-46);
	hands(58, 3, 94, 3, 0, blue, false);
	disc(4, blue);
}

static void on_draw(lv_event_t *e)
{
	static void (*const draw[DIAL_COUNT])(void) = {draw_bill, draw_rams, draw_station,
						       draw_tangente};

	fg = state.light ? 0x000000 : 0xffffff;
	bg = state.light ? 0xffffff : 0x000000;
	dim = state.light ? 0x555555 : 0xaaaaaa;
	draw_fill(e, bg);
	draw[state.style]();
}

lv_obj_t *dial_create(void)
{
	dial = lv_obj_create(lv_screen_active());
	lv_obj_remove_style_all(dial);
	lv_obj_set_size(dial, LV_PCT(100), LV_PCT(100));
	lv_obj_add_event_cb(dial, on_draw, LV_EVENT_DRAW_MAIN, NULL);
	return dial;
}

void dial_set(const struct dial_state *new_state)
{
	state = *new_state;
	lv_obj_invalidate(dial);
}
