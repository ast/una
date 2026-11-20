/*
 * Hardware watchdog. A hang or a fatal error resets the watch; if the image
 * running is one MCUboot was only asked to test, that reset puts the previous
 * image back.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(watchdog, LOG_LEVEL_INF);

#define TIMEOUT_MS 8000

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int channel;

/* From the system work queue: fed only while the kernel still schedules. */
static void feed(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(feed_work, feed);

static void feed(struct k_work *work)
{
	wdt_feed(wdt, channel);
	k_work_schedule(&feed_work, K_MSEC(TIMEOUT_MS / 4));
}

static int watchdog_start(void)
{
	const struct wdt_timeout_cfg cfg = {
		.window.max = TIMEOUT_MS,
		.flags = WDT_FLAG_RESET_SOC,
	};

	if (!device_is_ready(wdt)) {
		return -ENODEV;
	}
	channel = wdt_install_timeout(wdt, &cfg);
	if (channel < 0 || wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG) < 0) {
		LOG_ERR("watchdog not started");
		return -EIO;
	}
	k_work_schedule(&feed_work, K_NO_WAIT);
	return 0;
}
SYS_INIT(watchdog_start, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
