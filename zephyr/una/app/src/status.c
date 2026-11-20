/*
 * Battery and charger at a glance.
 */

#include <stdlib.h>

#include <zephyr/kernel.h>

#include "power.h"
#include "ui.h"

static lv_obj_t *percent_label;
static lv_obj_t *detail_label;

static void status_enter(void)
{
	lv_label_set_text(ui_label(&lv_font_montserrat_14, 0xaaaaaa, -92), "Battery");
	percent_label = ui_label(&lv_font_montserrat_48, 0xffffff, -40);
	detail_label = ui_label(&lv_font_montserrat_20, 0xffffff, 40);
}

static uint32_t status_update(void)
{
	/* PCA9420 charger states; 0 is idle. */
	static const char *const charging[] = {[1] = "charging", [2] = "charging",
					       [3] = "charging", [4] = "charging",
					       [5] = "topping off", [6] = "full"};
	struct power_status st;
	const char *source = "on battery";
	int64_t up_min = k_uptime_get() / 60000;

	power_last(&st);
	if (st.usb_power) {
		source = "on USB";
		if (st.charger_state < ARRAY_SIZE(charging) && charging[st.charger_state]) {
			source = charging[st.charger_state];
		}
	}
	lv_label_set_text_fmt(percent_label, "%d%%", st.percent);
	lv_label_set_text_fmt(detail_label, "%s\n%d.%02d V  %s%d.%d mA\nup %dh %02dm", source,
			      st.millivolts / 1000, st.millivolts % 1000 / 10,
			      st.microamps < 0 ? "-" : "", abs(st.microamps) / 1000,
			      abs(st.microamps) % 1000 / 100,
			      (int)(up_min / 60), (int)(up_min % 60));
	return 5000;
}

static void status_key(enum ui_key key, bool held)
{
	ARG_UNUSED(key);
	ARG_UNUSED(held);

	ui_pop();
}

const struct ui_screen status_screen = {
	.name = "status",
	.enter = status_enter,
	.update = status_update,
	.key = status_key,
};
