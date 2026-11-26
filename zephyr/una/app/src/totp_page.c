/*
 * The codes page. In the ring it only says how many there are and costs
 * nothing; SELECT goes in to the codes themselves, one at a time, with a
 * ring along the edge that closes as the code runs out.
 */
#include <stdio.h>

#include <zephyr/kernel.h>

#include "clock.h"
#include "draw.h"
#include "totp.h"
#include "ui.h"

#define SMALL (&lv_font_montserrat_12)
#define TEXT  (&lv_font_montserrat_20)
#define LARGE (&lv_font_montserrat_48)

/* A code left on the screen goes away: a frame a second is not free. */
#define SHOWN_MS 60000
/* How often the clock is asked whether the second has changed. */
#define POLL_MS  250
/* The ring turns amber when this little is left. */
#define LAST_S   5

static lv_obj_t *codes_canvas;
static lv_obj_t *code_canvas;
static int shown; /* which one */
static int64_t touched_ms;
static time_t painted_utc;

/* One code */

static void code_paint(void)
{
	char name[TOTP_NAME_MAX + 1];
	char text[16];
	uint32_t code;
	time_t utc = clock_utc();
	int count = totp_count();
	int elapsed = utc % TOTP_STEP_S;

	if (utc == 0 || !totp_get(shown, utc, name, &code)) {
		return;
	}
	draw_text(0, -52, name, TEXT, DRAW_WHITE, DRAW_CENTRE);
	snprintf(text, sizeof(text), "%03u %03u", code / 1000, code % 1000);
	draw_text(0, -6, text, LARGE, DRAW_GREEN, DRAW_CENTRE);
	if (count > 1) {
		snprintf(text, sizeof(text), "%d/%d", shown + 1, count);
		draw_text(0, 44, text, SMALL, DRAW_DIM, DRAW_CENTRE);
	}
	/* From twelve o'clock; closed during the code's last second. */
	draw_arc(0, 0, 117, 4, 270, 270 + (elapsed + 1) * 360 / TOTP_STEP_S,
		 elapsed >= TOTP_STEP_S - LAST_S ? DRAW_AMBER : DRAW_GREEN);
}

static void code_enter(void)
{
	touched_ms = k_uptime_get();
	painted_utc = 0;
	code_canvas = draw_create(code_paint);
}

static uint32_t code_update(void)
{
	time_t utc = clock_utc();

	if (utc == 0 || totp_count() == 0 || k_uptime_get() - touched_ms >= SHOWN_MS) {
		ui_pop();
		return UINT32_MAX;
	}
	if (shown >= totp_count()) {
		shown = 0;
	}
	/* The RTC has whole seconds; a frame only when it has a new one. */
	if (utc != painted_utc) {
		painted_utc = utc;
		lv_obj_invalidate(code_canvas);
	}
	return POLL_MS;
}

static bool code_key(enum ui_key key, bool held)
{
	int count = totp_count();

	ARG_UNUSED(held);
	if (key == UI_KEY_BACK || count == 0) {
		return false;
	}
	if (key == UI_KEY_UP) {
		shown = (shown + count - 1) % count;
	} else if (key == UI_KEY_DOWN) {
		shown = (shown + 1) % count;
	}
	touched_ms = k_uptime_get();
	painted_utc = 0;
	return true;
}

static const struct ui_screen code_screen = {
	.name = "code",
	.enter = code_enter,
	.update = code_update,
	.key = code_key,
};

/* The page in the ring */

static void codes_paint(void)
{
	char text[8];
	int count = totp_count();

	draw_text(0, -62, "CODES", SMALL, DRAW_DIM, DRAW_CENTRE);
	if (count == 0) {
		draw_text(0, 0, "NONE", TEXT, DRAW_GREY, DRAW_CENTRE);
		draw_text(0, 30, "una-z totp add", SMALL, DRAW_DIM, DRAW_CENTRE);
	} else if (clock_utc() == 0) {
		draw_text(0, 0, "CLOCK NOT SET", TEXT, DRAW_AMBER, DRAW_CENTRE);
	} else {
		snprintf(text, sizeof(text), "%d", count);
		draw_text(0, -14, text, LARGE, DRAW_WHITE, DRAW_CENTRE);
		draw_text(0, 24, "SELECT TO SHOW", SMALL, DRAW_DIM, DRAW_CENTRE);
	}
}

static void codes_enter(void)
{
	codes_canvas = draw_create(codes_paint);
}

/* Only when asked to: one was added or removed, or the clock was set. */
static uint32_t codes_update(void)
{
	lv_obj_invalidate(codes_canvas);
	return UINT32_MAX;
}

static bool codes_key(enum ui_key key, bool held)
{
	if (key == UI_KEY_SELECT && !held && totp_count() > 0 && clock_utc() != 0) {
		ui_push(&code_screen);
		return true;
	}
	return false;
}

const struct ui_screen codes_screen = {
	.name = "codes",
	.enter = codes_enter,
	.update = codes_update,
	.key = codes_key,
};
