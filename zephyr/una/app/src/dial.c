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

#define RED    0xff0000
#define YELLOW 0xffaa00

static struct dial_state state;
static lv_obj_t *dial;

/* Set at the start of a draw. */
static lv_layer_t *layer;
static int cx, cy;
static uint32_t fg, bg, dim;

const char *dial_name(enum dial_style style)
{
	static const char *const names[DIAL_COUNT] = {"Bill", "Rams", "Station", "Tangente"};

	return names[style];
}

/* A point `r` from the centre, `deg` clockwise from twelve. */
static lv_point_precise_t polar(int deg, int r)
{
	return (lv_point_precise_t){
		.x = cx + ((lv_trigo_sin(deg) * r + 16384) >> 15),
		.y = cy - ((lv_trigo_cos(deg) * r + 16384) >> 15),
	};
}

/* A line along a radius, from r1 to r2; negative is the other side. */
static void radial(int deg, int r1, int r2, int width, uint32_t rgb, bool round)
{
	lv_draw_line_dsc_t dsc;

	lv_draw_line_dsc_init(&dsc);
	dsc.p1 = polar(deg, r1);
	dsc.p2 = polar(deg, r2);
	dsc.width = width;
	dsc.color = lv_color_hex(rgb);
	dsc.round_start = round;
	dsc.round_end = round;
	lv_draw_line(layer, &dsc);
}

/* Angles as LVGL has them: clockwise from three o'clock. */
static void arc(int x, int y, int radius, int width, int start, int end, uint32_t rgb)
{
	lv_draw_arc_dsc_t dsc;

	lv_draw_arc_dsc_init(&dsc);
	dsc.center.x = cx + x;
	dsc.center.y = cy + y;
	dsc.radius = radius;
	dsc.width = width;
	dsc.start_angle = start;
	dsc.end_angle = end;
	dsc.color = lv_color_hex(rgb);
	lv_draw_arc(layer, &dsc);
}

static void disc(int radius, uint32_t rgb)
{
	arc(0, 0, radius, radius, 0, 360, rgb);
}

/* Text centred on a point given from the dial's centre. */
static void text(int x, int y, const char *str, const lv_font_t *font, uint32_t rgb)
{
	lv_draw_label_dsc_t dsc;
	lv_point_t size;
	lv_area_t area;

	lv_text_get_size(&size, str, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	area.x1 = cx + x - size.x / 2;
	area.y1 = cy + y - size.y / 2;
	area.x2 = area.x1 + size.x;
	area.y2 = area.y1 + size.y;

	lv_draw_label_dsc_init(&dsc);
	dsc.text = str;
	/* The string is the caller's and gone before the frame is drawn. */
	dsc.text_local = 1;
	dsc.font = font;
	dsc.color = lv_color_hex(rgb);
	lv_draw_label(layer, &dsc, &area);
}

static void ticks(int every_deg, int skip_deg, int r1, int r2, int width, uint32_t rgb)
{
	for (int deg = 0; deg < 360; deg += every_deg) {
		if (skip_deg == 0 || deg % skip_deg != 0) {
			radial(deg, r1, r2, width, rgb, false);
		}
	}
}

static void numeral(int hour, int r)
{
	lv_point_precise_t p = polar(hour * 30, r);
	char str[3];

	snprintf(str, sizeof(str), "%d", hour);
	text(p.x - cx, p.y - cy, str, &lv_font_montserrat_20, fg);
}

static void hands(int hour_len, int hour_width, int minute_len, int minute_width, int tail,
		  uint32_t rgb, bool round)
{
	int hour_deg = state.hour % 12 * 30 + state.minute / 2;

	radial(hour_deg, -tail, hour_len, hour_width, rgb, round);
	radial(state.minute * 6, -tail, minute_len, minute_width, rgb, round);
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
	text(0, y, str, &lv_font_montserrat_14, battery_colour(dim));
}

static void date(int x, bool window)
{
	char str[4];

	if (state.day == 0) {
		return;
	}
	snprintf(str, sizeof(str), "%d", state.day);
	if (window) {
		lv_draw_rect_dsc_t dsc;
		lv_area_t area = {cx + x - 13, cy - 10, cx + x + 13, cy + 10};

		lv_draw_rect_dsc_init(&dsc);
		dsc.bg_opa = LV_OPA_TRANSP;
		dsc.border_color = lv_color_hex(dim);
		dsc.border_width = 1;
		lv_draw_rect(layer, &dsc, &area);
	}
	text(x, 0, str, &lv_font_montserrat_14, window ? fg : dim);
}

static void alarm(int y)
{
	char str[16];

	if (state.alarm_on) {
		snprintf(str, sizeof(str), LV_SYMBOL_BELL " %02d:%02d", state.alarm_hour,
			 state.alarm_minute);
		text(0, y, str, &lv_font_montserrat_14, dim);
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
	arc(0, 0, 50, 2, 90 - span / 2, 90 + span / 2, dim);
	arc(0, 0, 51, 4, 90 + span / 2 - level, 90 + span / 2, battery_colour(fg));
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
			radial(hour * 30, 80, 98, 2, fg, false);
		} else if (hour != 6) {
			numeral(hour, 89);
		}
	}
	arc(0, sub_y, sub_r, 1, 0, 360, dim);
	arc(0, sub_y, sub_r - 3, 3, 135, 135 + sweep * state.battery / 100, battery_colour(fg));
	snprintf(str, sizeof(str), "%d", state.battery);
	text(0, sub_y, str, &lv_font_montserrat_14, battery_colour(dim));
	date(54, false);
	alarm(-46);
	hands(58, 3, 94, 3, 0, blue, false);
	disc(4, blue);
}

static void on_draw(lv_event_t *e)
{
	static void (*const draw[DIAL_COUNT])(void) = {draw_bill, draw_rams, draw_station,
						       draw_tangente};
	lv_obj_t *obj = lv_event_get_target(e);
	lv_draw_rect_dsc_t rect;
	lv_area_t area;

	lv_obj_get_coords(obj, &area);
	layer = lv_event_get_layer(e);
	cx = (area.x1 + area.x2 + 1) / 2;
	cy = (area.y1 + area.y2 + 1) / 2;
	fg = state.light ? 0x000000 : 0xffffff;
	bg = state.light ? 0xffffff : 0x000000;
	dim = state.light ? 0x555555 : 0xaaaaaa;

	lv_draw_rect_dsc_init(&rect);
	rect.bg_color = lv_color_hex(bg);
	lv_draw_rect(layer, &rect, &area);

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
