/*
 * Drawing straight onto an object: see draw.h.
 */

#include "draw.h"

/* Set by draw_fill at the start of a draw. */
static lv_layer_t *layer;
static int cx, cy;

static void on_draw(lv_event_t *e)
{
	void (*paint)(void) = lv_event_get_user_data(e);

	draw_fill(e, 0x000000);
	paint();
}

lv_obj_t *draw_create(void (*paint)(void))
{
	lv_obj_t *obj = lv_obj_create(lv_screen_active());

	lv_obj_remove_style_all(obj);
	lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
	lv_obj_add_event_cb(obj, on_draw, LV_EVENT_DRAW_MAIN, paint);
	return obj;
}

void draw_fill(lv_event_t *e, uint32_t rgb)
{
	lv_obj_t *obj = lv_event_get_target(e);
	lv_draw_rect_dsc_t rect;
	lv_area_t area;

	lv_obj_get_coords(obj, &area);
	layer = lv_event_get_layer(e);
	cx = (area.x1 + area.x2 + 1) / 2;
	cy = (area.y1 + area.y2 + 1) / 2;

	lv_draw_rect_dsc_init(&rect);
	rect.bg_color = lv_color_hex(rgb);
	lv_draw_rect(layer, &rect, &area);
}

lv_point_precise_t draw_polar(int deg, int r)
{
	return (lv_point_precise_t){
		.x = cx + ((lv_trigo_sin(deg) * r + 16384) >> 15),
		.y = cy - ((lv_trigo_cos(deg) * r + 16384) >> 15),
	};
}

static void line(lv_point_precise_t p1, lv_point_precise_t p2, int width, uint32_t rgb, bool round)
{
	lv_draw_line_dsc_t dsc;

	lv_draw_line_dsc_init(&dsc);
	dsc.p1 = p1;
	dsc.p2 = p2;
	dsc.width = width;
	dsc.color = lv_color_hex(rgb);
	dsc.round_start = round;
	dsc.round_end = round;
	lv_draw_line(layer, &dsc);
}

void draw_line(int x1, int y1, int x2, int y2, int width, uint32_t rgb)
{
	line((lv_point_precise_t){cx + x1, cy + y1}, (lv_point_precise_t){cx + x2, cy + y2}, width,
	     rgb, false);
}

void draw_radial(int deg, int r1, int r2, int width, uint32_t rgb, bool round)
{
	line(draw_polar(deg, r1), draw_polar(deg, r2), width, rgb, round);
}

void draw_arc(int x, int y, int radius, int width, int start, int end, uint32_t rgb)
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

void draw_box(int x, int y, int w, int h, uint32_t rgb, bool filled)
{
	lv_draw_rect_dsc_t dsc;
	lv_area_t area = {cx + x - w / 2, cy + y - h / 2, cx + x + (w - 1) / 2,
			  cy + y + (h - 1) / 2};

	lv_draw_rect_dsc_init(&dsc);
	if (filled) {
		dsc.bg_color = lv_color_hex(rgb);
	} else {
		dsc.bg_opa = LV_OPA_TRANSP;
		dsc.border_color = lv_color_hex(rgb);
		dsc.border_width = 1;
	}
	lv_draw_rect(layer, &dsc, &area);
}

int draw_text(int x, int y, const char *str, const lv_font_t *font, uint32_t rgb,
	      enum draw_align align)
{
	lv_draw_label_dsc_t dsc;
	lv_point_t size;
	lv_area_t area;

	lv_text_get_size(&size, str, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	area.x1 = cx + x - (align == DRAW_LEFT ? 0 : align == DRAW_CENTRE ? size.x / 2 : size.x);
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
	return size.x;
}
