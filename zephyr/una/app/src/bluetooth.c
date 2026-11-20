/*
 * Bluetooth bring-up. Runs in its own thread: enabling the controller waits
 * on the radio chip and must not hold up the UI or the shell.
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bluetooth, LOG_LEVEL_INF);

/* Experiment: with this at 0 the radio chip is held in reset instead. */
#define BT_AUTOSTART 0

static void bluetooth_thread(void *p1, void *p2, void *p3)
{
	int err;

	if (!BT_AUTOSTART) {
		static const struct gpio_dt_spec reset =
			GPIO_DT_SPEC_GET(DT_CHOSEN(zephyr_bt_hci), reset_gpios);

		gpio_pin_configure_dt(&reset, GPIO_OUTPUT_ACTIVE);
		LOG_INF("Bluetooth chip held in reset");
		return;
	}

	err = bt_enable(NULL);

	if (err != 0) {
		LOG_ERR("Bluetooth did not start: %d", err);
		return;
	}
	LOG_INF("Bluetooth ready");
}
K_THREAD_DEFINE(bluetooth_tid, 4096, bluetooth_thread, NULL, NULL, NULL, 7, 0, 3000);
