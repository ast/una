/*
 * Zephyr on the UNA Watch: a test screen, a shell on the USB serial port and
 * mcumgr for loading the next build.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/version.h>

#include <lvgl.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const struct device *const display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct gpio_dt_spec backlight = GPIO_DT_SPEC_GET(DT_ALIAS(backlight), gpios);

static lv_obj_t *key_label;
static atomic_t last_key;

static void on_input(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY) {
		return;
	}
	LOG_INF("key %u %s", evt->code, evt->value ? "down" : "up");
	if (evt->value) {
		atomic_set(&last_key, evt->code);
	}
}
INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);

static void build_screen(void)
{
	/* The panel has 4 levels per colour: these are all exact. */
	static const uint32_t bars[] = {0xff0000, 0x00ff00, 0x0000ff, 0xffff00,
					0xff00ff, 0x00ffff, 0xffffff, 0x555555};
	lv_obj_t *scr = lv_screen_active();
	lv_obj_t *label;

	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

	for (int i = 0; i < ARRAY_SIZE(bars); i++) {
		lv_obj_t *bar = lv_obj_create(scr);

		lv_obj_remove_style_all(bar);
		lv_obj_set_size(bar, 240 / ARRAY_SIZE(bars), 40);
		lv_obj_set_pos(bar, i * (240 / ARRAY_SIZE(bars)), 150);
		lv_obj_set_style_bg_color(bar, lv_color_hex(bars[i]), 0);
		lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	}

	label = lv_label_create(scr);
	lv_label_set_text(label, "Zephyr");
	lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
	lv_obj_set_style_text_color(label, lv_color_white(), 0);
	lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 40);

	label = lv_label_create(scr);
	lv_label_set_text(label, KERNEL_VERSION_STRING);
	lv_obj_set_style_text_color(label, lv_color_hex(0xaaaaaa), 0);
	lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 80);

	key_label = lv_label_create(scr);
	lv_label_set_text(key_label, "press a button");
	lv_obj_set_style_text_color(key_label, lv_color_hex(0xffff00), 0);
	lv_obj_align(key_label, LV_ALIGN_TOP_MID, 0, 110);
}

int main(void)
{
	gpio_pin_configure_dt(&backlight, GPIO_OUTPUT_ACTIVE);

	if (!device_is_ready(display)) {
		LOG_ERR("display not ready");
		return 0;
	}

	build_screen();
	display_blanking_off(display);

	while (true) {
		atomic_val_t key = atomic_clear(&last_key);

		if (key != 0) {
			lv_label_set_text_fmt(key_label, "SW%d", (int)(key - INPUT_KEY_1 + 1));
		}
		/*
		 * LVGL answers "never" (UINT32_MAX) when the screen is idle;
		 * keep polling the buttons, and never spin.
		 */
		k_msleep(CLAMP(lv_timer_handler(), 5, 50));
	}
	return 0;
}
