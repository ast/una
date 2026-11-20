/*
 * The UI: a stack of screens, the four buttons and the backlight.
 *
 * Everything that touches LVGL runs in the main thread. Buttons and other
 * threads reach it through a message queue, and the thread sleeps until the
 * next message or the next thing to draw, so the SoC can stay in Stop mode.
 */

#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>

#include "power.h"
#include "ui.h"

LOG_MODULE_REGISTER(ui, LOG_LEVEL_INF);

static const struct device *const display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct gpio_dt_spec backlight = GPIO_DT_SPEC_GET(DT_ALIAS(backlight), gpios);

atomic_t usage_key_presses;
atomic_t usage_backlight_ms;

enum event_type {
	EVENT_PRESS, /* a button went down: light up */
	EVENT_KEY,
	EVENT_REFRESH,
	EVENT_SHOW,
};

struct event {
	uint8_t type;
	uint8_t key;
	bool held;
	const struct ui_screen *screen;
};

K_MSGQ_DEFINE(events, sizeof(struct event), 16, 4);

/*
 * The board's longpress node turns each button into one of two codes: A-D
 * when released early, W-Z once held.
 */
static void on_input(struct input_event *evt, void *user_data)
{
	static const uint16_t tapped[] = {INPUT_KEY_A, INPUT_KEY_B, INPUT_KEY_C, INPUT_KEY_D};
	static const uint16_t held[] = {INPUT_KEY_W, INPUT_KEY_X, INPUT_KEY_Y, INPUT_KEY_Z};
	struct event ev = {.type = EVENT_KEY};

	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY || !evt->value) {
		return;
	}
	if (evt->code >= INPUT_KEY_1 && evt->code <= INPUT_KEY_4) {
		atomic_inc(&usage_key_presses);
		ev.type = EVENT_PRESS;
		k_msgq_put(&events, &ev, K_NO_WAIT);
		return;
	}
	for (int i = 0; i < ARRAY_SIZE(tapped); i++) {
		if (evt->code == tapped[i] || evt->code == held[i]) {
			ev.key = i;
			ev.held = evt->code == held[i];
			k_msgq_put(&events, &ev, K_NO_WAIT);
			return;
		}
	}
}
INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);

void ui_refresh(void)
{
	struct event ev = {.type = EVENT_REFRESH};

	k_msgq_put(&events, &ev, K_NO_WAIT);
}

void ui_show(const struct ui_screen *screen)
{
	struct event ev = {.type = EVENT_SHOW, .screen = screen};

	k_msgq_put(&events, &ev, K_NO_WAIT);
}

/* Backlight */

static int32_t backlight_s = 5;
static int64_t backlight_until;

int ui_backlight_s(void)
{
	return backlight_s;
}

void ui_set_backlight_s(int seconds)
{
	backlight_s = seconds;
	settings_save_one("ui/backlight_s", &backlight_s, sizeof(backlight_s));
}

static int ui_setting_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "backlight_s", NULL) && len == sizeof(backlight_s)) {
		return read_cb(cb_arg, &backlight_s, sizeof(backlight_s)) < 0 ? -EIO : 0;
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(ui, "ui", NULL, ui_setting_set, NULL, NULL);

/* Screens */

static const struct ui_screen *stack[6] = {&face_screen};
static int depth = 1;
static uint32_t loops;

static void show_top(void)
{
	lv_obj_clean(lv_screen_active());
	stack[depth - 1]->enter();
}

void ui_push(const struct ui_screen *screen)
{
	if (depth == ARRAY_SIZE(stack)) {
		LOG_ERR("screen stack full");
		return;
	}
	stack[depth++] = screen;
	show_top();
}

void ui_pop(void)
{
	if (depth > 1) {
		depth--;
		show_top();
	}
}

void ui_home(void)
{
	if (depth > 1) {
		depth = 1;
		show_top();
	}
}

void ui_run(void)
{
	int64_t lit_since = k_uptime_get();
	int64_t update_at = 0;
	bool lit = true;

	gpio_pin_configure_dt(&backlight, GPIO_OUTPUT_INACTIVE);
	if (!device_is_ready(display)) {
		LOG_ERR("display not ready");
		return;
	}

	lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
	backlight_until = k_uptime_get() + backlight_s * 1000;
	show_top();
	display_blanking_off(display);

	while (true) {
		struct event ev;
		uint32_t wait_ms;
		int64_t now = k_uptime_get();

		if (now >= update_at) {
			uint32_t next_ms = stack[depth - 1]->update();

			update_at = next_ms == UINT32_MAX ? INT64_MAX : now + next_ms;
		}
		/*
		 * LVGL answers "never" (UINT32_MAX) once the frame is drawn. Not
		 * inside MIN(), which would call it twice and keep the second
		 * answer.
		 */
		wait_ms = lv_timer_handler();
		wait_ms = MIN(wait_ms, update_at - now);

		now = k_uptime_get();
		if (lit) {
			atomic_add(&usage_backlight_ms, now - lit_since);
		}
		lit = now < backlight_until;
		lit_since = now;
		gpio_pin_set_dt(&backlight, lit);
		if (lit) {
			wait_ms = MIN(wait_ms, backlight_until - now);
		}
		loops++;

		if (k_msgq_get(&events, &ev, K_MSEC(MAX(wait_ms, 5))) != 0) {
			continue;
		}
		do {
			if (ev.type == EVENT_PRESS || ev.type == EVENT_SHOW) {
				backlight_until = k_uptime_get() + backlight_s * 1000;
			}
			if (ev.type == EVENT_SHOW) {
				if (stack[depth - 1] != ev.screen) {
					ui_push(ev.screen);
				}
			} else if (ev.type == EVENT_KEY) {
				if (ev.key == UI_KEY_BACK && ev.held) {
					ui_home();
				} else {
					stack[depth - 1]->key(ev.key, ev.held);
				}
			}
		} while (k_msgq_get(&events, &ev, K_NO_WAIT) == 0);
		/* Whatever it was, the screen may have something new to show. */
		update_at = 0;
	}
}

/* Widgets */

lv_obj_t *ui_label(const lv_font_t *font, uint32_t rgb, int y)
{
	lv_obj_t *label = lv_label_create(lv_screen_active());

	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(rgb), 0);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
	lv_label_set_text(label, "");
	return label;
}

#define LIST_ROW_PITCH 32

void ui_list_create(struct ui_list *list, const char *title, int count)
{
	/* A short list sits in the middle of the screen. */
	int first_y = 10 - (MIN(count, UI_LIST_ROWS) - 1) * LIST_ROW_PITCH / 2;

	lv_label_set_text(ui_label(&lv_font_montserrat_14, 0xaaaaaa, -92), title);

	for (int i = 0; i < UI_LIST_ROWS; i++) {
		lv_obj_t *row = ui_label(&lv_font_montserrat_20, 0xffffff, first_y + i * LIST_ROW_PITCH);

		lv_obj_set_width(row, 184);
		lv_obj_set_style_radius(row, 14, 0);
		lv_obj_set_style_pad_ver(row, 3, 0);
		lv_obj_set_style_bg_color(row, lv_color_white(), 0);
		lv_label_set_long_mode(row, LV_LABEL_LONG_MODE_CLIP);
		list->rows[i] = row;
	}
	list->count = count;
	list->selected = CLAMP(list->selected, 0, count - 1);
}

void ui_list_show(struct ui_list *list, const char *(*text)(int index))
{
	/* The selection stays in the middle row where the list allows it. */
	int top = CLAMP(list->selected - UI_LIST_ROWS / 2, 0, MAX(list->count - UI_LIST_ROWS, 0));

	for (int i = 0; i < UI_LIST_ROWS; i++) {
		lv_obj_t *row = list->rows[i];
		int index = top + i;
		bool selected = index == list->selected;

		lv_label_set_text(row, index < list->count ? text(index) : "");
		lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_obj_set_style_text_color(row, selected ? lv_color_black() : lv_color_white(), 0);
	}
}

bool ui_list_key(struct ui_list *list, enum ui_key key)
{
	if (key == UI_KEY_UP) {
		list->selected = (list->selected + list->count - 1) % list->count;
	} else if (key == UI_KEY_DOWN) {
		list->selected = (list->selected + 1) % list->count;
	} else {
		return false;
	}
	return true;
}

/* Confirmation */

static const char *confirm_question;
static void (*confirm_yes)(void);

static void confirm_enter(void)
{
	lv_label_set_text(ui_label(&lv_font_montserrat_20, 0xffffff, -20), confirm_question);
	lv_label_set_text(ui_label(&lv_font_montserrat_14, 0xaaaaaa, 40),
			  "top right: yes\nbottom right: no");
}

static uint32_t confirm_update(void)
{
	return UINT32_MAX;
}

static void confirm_key(enum ui_key key, bool held)
{
	if (key == UI_KEY_SELECT && !held) {
		ui_pop();
		confirm_yes();
	} else if (key == UI_KEY_BACK) {
		ui_pop();
	}
}

static const struct ui_screen confirm_screen = {
	.name = "confirm",
	.enter = confirm_enter,
	.update = confirm_update,
	.key = confirm_key,
};

void ui_confirm(const char *question, void (*yes)(void))
{
	confirm_question = question;
	confirm_yes = yes;
	ui_push(&confirm_screen);
}

static int cmd_ui(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "screen: %s (depth %d), backlight %d s, %u loops", stack[depth - 1]->name,
		    depth, (int)backlight_s, loops);
	return 0;
}
SHELL_CMD_REGISTER(ui, NULL, "Current screen and how often the UI thread has run", cmd_ui);
