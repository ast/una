/*
 * Wall-clock time: the RTC in UTC plus a stored offset.
 */

/* gmtime_r, for the simulator's host C library. */
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <time.h>

#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/timeutil.h>

#include "alarm.h"
#include "clock.h"
#include "ui.h"

LOG_MODULE_REGISTER(clock, LOG_LEVEL_INF);

static const struct device *const rtc = DEVICE_DT_GET(DT_ALIAS(rtc));
static int32_t tz_min;

bool clock_local(struct tm *out)
{
	struct rtc_time utc;
	time_t t;

	if (rtc_get_time(rtc, &utc) != 0) {
		return false;
	}
	t = timeutil_timegm(rtc_time_to_tm(&utc)) + tz_min * 60;
	gmtime_r(&t, out);
	return true;
}

time_t clock_utc(void)
{
	struct rtc_time utc;

	if (rtc_get_time(rtc, &utc) != 0) {
		return 0;
	}
	return timeutil_timegm(rtc_time_to_tm(&utc));
}

int clock_utc_offset(void)
{
	return tz_min * 60;
}

static int clock_setting_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "tz_min", NULL) && len == sizeof(tz_min)) {
		return read_cb(cb_arg, &tz_min, sizeof(tz_min)) < 0 ? -EIO : 0;
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(una, "una", NULL, clock_setting_set, NULL, NULL);

static int clock_init(void)
{
	int err = settings_subsys_init();

	if (err == 0) {
		err = settings_load();
	}
	if (err != 0) {
		LOG_ERR("settings not loaded: %d", err);
	}
	return 0;
}
SYS_INIT(clock_init, APPLICATION, 96);

int clock_set(time_t utc_s, int offset_min)
{
	struct rtc_time rt = {0};
	struct tm utc;

	gmtime_r(&utc_s, &utc);
	rt.tm_year = utc.tm_year;
	rt.tm_mon = utc.tm_mon;
	rt.tm_mday = utc.tm_mday;
	rt.tm_hour = utc.tm_hour;
	rt.tm_min = utc.tm_min;
	rt.tm_sec = utc.tm_sec;
	rt.tm_wday = utc.tm_wday;
	if (rtc_set_time(rtc, &rt) != 0) {
		return -EIO;
	}
	if (offset_min != tz_min) {
		tz_min = offset_min;
		settings_save_one("una/tz_min", &tz_min, sizeof(tz_min));
	}
	alarm_clock_changed();
	ui_refresh();
	return 0;
}

/* clock [set <seconds since 1970 UTC> <offset minutes>] */
static int cmd_clock(const struct shell *sh, size_t argc, char **argv)
{
	struct tm local;

	if (argc == 4) {
		if (clock_set(strtoll(argv[2], NULL, 10), atoi(argv[3])) != 0) {
			shell_error(sh, "the RTC refused that time");
			return -EIO;
		}
	} else if (argc != 1) {
		shell_error(sh, "usage: clock [set <seconds since 1970 UTC> <offset minutes>]");
		return -EINVAL;
	}

	if (!clock_local(&local)) {
		shell_print(sh, "not set");
		return 0;
	}
	shell_print(sh, "%04d-%02d-%02d %02d:%02d:%02d local (UTC%+d min)", local.tm_year + 1900,
		    local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec,
		    (int)tz_min);
	return 0;
}
SHELL_CMD_ARG_REGISTER(clock, NULL, "Show local time, or: clock set <seconds since 1970 UTC> <offset minutes>",
		       cmd_clock, 1, 3);
