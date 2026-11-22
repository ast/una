/*
 * What the phone was notified of. The page shows the latest; SELECT goes in,
 * and UP and DOWN then go through the ones before it. One that arrives is
 * shown on top of whatever was there, already gone into.
 *
 * The watch keeps the last few and removes none itself: one dismissed on the
 * phone goes from here too.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "clock.h"
#include "draw.h"
#include "phone.h"
#include "ui.h"

static lv_obj_t *head_label, *title_label, *body_label, *foot_label;
static struct phone_note note;
static int cursor;
static bool inside;
static uint32_t shown_version;

static void messages_enter(void)
{
	head_label = ui_label(&font_text_14, DRAW_DIM, -92);
	title_label = ui_label(&font_text_20, DRAW_WHITE, -62);
	lv_obj_set_width(title_label, 176);
	lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_height(title_label, lv_font_get_line_height(&font_text_20));

	body_label = ui_label(&font_text_14, DRAW_WHITE, 14);
	lv_obj_set_size(body_label, 204, 6 * lv_font_get_line_height(&font_text_14));
	lv_label_set_long_mode(body_label, LV_LABEL_LONG_MODE_DOTS);

	foot_label = ui_label(&font_text_14, DRAW_DIM, 92);
	shown_version = phone_notes_version() - 1;
}

/* Gone into from the page: the latest first. */
static void message_enter(void)
{
	inside = true;
	cursor = 0;
	messages_enter();
}

static void page_enter(void)
{
	inside = false;
	cursor = 0;
	messages_enter();
}

static uint32_t messages_update(void)
{
	int count = phone_notes();
	char text[32];

	if (phone_notes_version() != shown_version) {
		/* One came or went: the latest is what to look at. */
		shown_version = phone_notes_version();
		cursor = 0;
	}
	if (!phone_note(cursor, &note)) {
		lv_label_set_text(head_label, "MESSAGES");
		lv_label_set_text(title_label, "");
		lv_label_set_text(body_label, "\n\nNone");
		lv_obj_set_style_text_color(body_label, lv_color_hex(DRAW_DIM), 0);
		lv_label_set_text(foot_label, "");
		return UINT32_MAX;
	}
	ui_readable(note.source);
	ui_readable(note.title);
	ui_readable(note.body);
	lv_obj_set_style_text_color(body_label, lv_color_hex(DRAW_WHITE), 0);
	lv_label_set_text(head_label, note.source[0] != '\0' ? note.source : "MESSAGE");
	lv_label_set_text(title_label, note.title);
	lv_label_set_text(body_label, note.body);

	/* When it came, and where among them this one is once gone into. */
	text[0] = '\0';
	if (note.at != 0) {
		int minute = (note.at + clock_utc_offset()) / 60 % (24 * 60);

		snprintf(text, sizeof(text), "%02d:%02d", minute / 60, minute % 60);
	}
	if (inside && count > 1) {
		lv_label_set_text_fmt(foot_label, "%s   %d/%d", text, cursor + 1, count);
	} else {
		lv_label_set_text(foot_label, text);
	}
	lv_obj_set_style_text_color(foot_label, lv_color_hex(inside ? DRAW_CYAN : DRAW_DIM), 0);
	return UINT32_MAX;
}

/* Gone into: UP is the one that came after, DOWN the one before. */
static bool step(enum ui_key key)
{
	int count = phone_notes();

	if (key == UI_KEY_UP && cursor > 0) {
		cursor--;
	} else if (key == UI_KEY_DOWN && cursor < count - 1) {
		cursor++;
	}
	return key == UI_KEY_UP || key == UI_KEY_DOWN || key == UI_KEY_SELECT;
}

static bool page_key(enum ui_key key, bool held)
{
	if (held) {
		return false;
	}
	if (!inside) {
		if (key != UI_KEY_SELECT || phone_notes() == 0) {
			return false;
		}
		inside = true;
		return true;
	}
	if (key == UI_KEY_BACK) {
		inside = false;
		cursor = 0;
		return true;
	}
	return step(key);
}

static bool message_key(enum ui_key key, bool held)
{
	return !held && step(key);
}

const struct ui_screen messages_screen = {
	.name = "messages",
	.enter = page_enter,
	.update = messages_update,
	.key = page_key,
};

const struct ui_screen message_screen = {
	.name = "message",
	.enter = message_enter,
	.update = messages_update,
	.key = message_key,
};
