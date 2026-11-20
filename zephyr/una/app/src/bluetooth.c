/*
 * Bluetooth bring-up. Runs in its own thread: enabling the controller waits
 * on the radio chip and must not hold up the UI or the shell.
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bluetooth, LOG_LEVEL_INF);

static void bluetooth_thread(void *p1, void *p2, void *p3)
{
	int err = bt_enable(NULL);

	if (err != 0) {
		LOG_ERR("Bluetooth did not start: %d", err);
		return;
	}
	LOG_INF("Bluetooth ready");
}
K_THREAD_DEFINE(bluetooth_tid, 4096, bluetooth_thread, NULL, NULL, NULL, 7, 0, 3000);
