#ifndef UNA_DRAW_H_
#define UNA_DRAW_H_

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

/*
 * Drawing straight onto an object from its LV_EVENT_DRAW_MAIN callback:
 * the dials and the system pages. Coordinates are from the object's centre.
 *
 * The panel has four levels per colour; these are colours it has, used the
 * same way on every page.
 */
#define DRAW_WHITE   0xffffff /* labels, units */
#define DRAW_GREEN   0x00ff00 /* a live value that is normal, a flow */
#define DRAW_AMBER   0xffaa00 /* caution */
#define DRAW_RED     0xff0000 /* warning */
#define DRAW_CYAN    0x00ffff /* what can be set here */
#define DRAW_MAGENTA 0xff55ff /* a target */
#define DRAW_GREY    0x555555 /* off, no data */
#define DRAW_DIM     0xaaaaaa

enum draw_align {
	DRAW_LEFT,
	DRAW_CENTRE,
	DRAW_RIGHT,
};

/* An object that fills the screen and is painted by `paint`. */
lv_obj_t *draw_create(void (*paint)(void));

/* Fills the object; the centre is where it is until the next fill. */
void draw_fill(lv_event_t *e, uint32_t rgb);

/* A point `r` from the centre, `deg` clockwise from twelve. */
lv_point_precise_t draw_polar(int deg, int r);

void draw_line(int x1, int y1, int x2, int y2, int width, uint32_t rgb);
/* A line along a radius, from r1 to r2; negative is the other side. */
void draw_radial(int deg, int r1, int r2, int width, uint32_t rgb, bool round);
/* Angles as LVGL has them: clockwise from three o'clock. */
void draw_arc(int x, int y, int radius, int width, int start, int end, uint32_t rgb);
/* A rectangle centred on a point: an outline, or filled. */
void draw_box(int x, int y, int w, int h, uint32_t rgb, bool filled);
/* Text on one line, vertically centred on y. Returns its width. */
int draw_text(int x, int y, const char *str, const lv_font_t *font, uint32_t rgb,
	      enum draw_align align);

#endif
