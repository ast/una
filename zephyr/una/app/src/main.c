/*
 * Zephyr on the UNA Watch: a watch face, a shell on the USB serial port and
 * mcumgr for loading the next build.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

#include "clock.h"
#include "power.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define BACKLIGHT_TIMEOUT_MS 5000

static const struct device *const display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct gpio_dt_spec backlight = GPIO_DT_SPEC_GET(DT_ALIAS(backlight), gpios);

static lv_obj_t *time_label;
static lv_obj_t *date_label;
static lv_obj_t *battery_label;
static int64_t backlight_until;
static K_SEM_DEFINE(wake, 0, 1);

atomic_t usage_key_presses;
atomic_t usage_backlight_ms;

static void on_input(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY) {
		return;
	}
	LOG_DBG("key %u %s", evt->code, evt->value ? "down" : "up");
	if (evt->value) {
		atomic_inc(&usage_key_presses);
		backlight_until = k_uptime_get() + BACKLIGHT_TIMEOUT_MS;
		k_sem_give(&wake);
	}
}
INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);

static void build_face(void)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

	date_label = lv_label_create(scr);
	lv_obj_set_style_text_font(date_label, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(date_label, lv_color_hex(0xaaaaaa), 0);
	lv_obj_align(date_label, LV_ALIGN_CENTER, 0, -52);

	time_label = lv_label_create(scr);
	lv_obj_set_style_text_font(time_label, &lv_font_montserrat_48, 0);
	lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
	lv_obj_align(time_label, LV_ALIGN_CENTER, 0, 0);

	battery_label = lv_label_create(scr);
	lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_20, 0);
	lv_obj_align(battery_label, LV_ALIGN_CENTER, 0, 54);
}

/* Returns the seconds left in the current minute. */
static int update_face(void)
{
	static const char *const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
					     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	struct power_status st;
	struct tm now;
	int left = 60;

	if (clock_local(&now)) {
		lv_label_set_text_fmt(time_label, "%02d:%02d", now.tm_hour, now.tm_min);
		lv_label_set_text_fmt(date_label, "%s %d %s", days[now.tm_wday], now.tm_mday,
				      months[now.tm_mon]);
		left = 60 - now.tm_sec;
	} else {
		lv_label_set_text(time_label, "--:--");
		lv_label_set_text(date_label, "clock not set");
	}

	power_last(&st);
	if (st.millivolts == 0) {
		/* No reading yet: the first comes a few seconds after boot. */
		lv_label_set_text(battery_label, "");
		return MIN(left, 2);
	}
	lv_label_set_text_fmt(battery_label, "%d%%%s", st.percent, st.usb_power ? " +" : "");
	/* The panel's four levels per colour: these three are exact. */
	lv_obj_set_style_text_color(battery_label,
				    lv_color_hex(st.percent <= 10   ? 0xff0000
						 : st.percent <= 25 ? 0xffff00
								    : 0x00ff00),
				    0);
	return left;
}

int main(void)
{
	int64_t loop_start;

	gpio_pin_configure_dt(&backlight, GPIO_OUTPUT_ACTIVE);

	if (!device_is_ready(display)) {
		LOG_ERR("display not ready");
		return 0;
	}

	backlight_until = k_uptime_get() + BACKLIGHT_TIMEOUT_MS;
	build_face();
	display_blanking_off(display);
	loop_start = k_uptime_get();

	while (true) {
		int minute_left_s = update_face();
		bool lit = k_uptime_get() < backlight_until;
		uint32_t next_ms;

		gpio_pin_set_dt(&backlight, lit);
		if (lit) {
			atomic_add(&usage_backlight_ms, k_uptime_get() - loop_start);
		}
		loop_start = k_uptime_get();

		/*
		 * LVGL answers "never" (UINT32_MAX) when the screen is idle.
		 * With the backlight off the next thing to draw is the next
		 * minute: sleep until then or a button, so the SoC can stay in
		 * Stop mode.
		 */
		next_ms = CLAMP(lv_timer_handler(), 5, 50);
		if (!lit) {
			next_ms = minute_left_s * 1000;
		}
		k_sem_take(&wake, K_MSEC(next_ms));
	}
	return 0;
}
