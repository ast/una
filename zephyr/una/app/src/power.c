/*
 * Battery, charger and power-off.
 *
 * The PCA9420 is driven register by register rather than through Zephyr's
 * regulator driver: the UNA bootloader has already set the four rails the
 * watch runs on, and nothing here should rewrite them. Register layout as in
 * Libs/Header/Hardware/PCA9420.hpp of una-kernel-public.
 */

#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/policy.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>

#include "power.h"

LOG_MODULE_REGISTER(power, LOG_LEVEL_INF);

#define PMIC_ADDR 0x61

#define PMIC_TOP_CNTL3    0x0c
#define PMIC_CHG_CNTL1    0x11
#define PMIC_CHG_STATUS_0 0x18
#define PMIC_CHG_STATUS_2 0x1a

#define TOP_CNTL3_MODE_MASK (BIT(3) | BIT(4))
/* Mode 2: only LDO1 stays on. The ON key brings mode 0 back. */
#define TOP_CNTL3_MODE_OFF  BIT(4)

#define CHG_STATUS_0_VIN_OK BIT(5)
#define CHG_STATUS_2_STATE  GENMASK(2, 0)

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c1));
static const struct device *const gauge = DEVICE_DT_GET(DT_ALIAS(fuel_gauge0));

static int32_t milli(const struct sensor_value *v)
{
	return v->val1 * 1000 + v->val2 / 1000;
}

#define GAUGE_ADDR    0x36
#define GAUGE_REP_SOC 0x06 /* the gauge's percentage, in 1/256 % */
#define GAUGE_MIX_CAP 0x0f /* the charge it reckons is in the cell */
#define GAUGE_QH      0x4d /* its plain coulomb counter */

static int gauge_read(uint8_t reg)
{
	uint8_t raw[2] = {0};

	(void)i2c_burst_read(i2c, GAUGE_ADDR, reg, raw, sizeof(raw));
	return sys_get_le16(raw);
}

static int gauge_qh(void)
{
	return (int16_t)gauge_read(GAUGE_QH);
}

/*
 * How full the battery is.
 *
 * The gauge's own percentage (RepSOC) is not it: its model of the cell is
 * full at a lower voltage than the 4.40 V this one is charged to, so it
 * reckons a full cell at about 108 % and reports 100 until the surplus is
 * used up, which took over a day on the wrist. Its estimate of the charge
 * in the cell (MixCap, coulomb counter and voltage combined) has no such
 * ceiling. So the percentage is that charge against what it was when the
 * charger last called the cell full, which is remembered; and the gauge's
 * own figure where it is the lower, towards empty, where it allows for the
 * load.
 */
static uint16_t full_raw; /* MixCap at full, 0.5 mAh a step; 0: not seen yet */

static int power_setting_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "full", NULL) && len == sizeof(full_raw)) {
		return read_cb(cb_arg, &full_raw, sizeof(full_raw)) < 0 ? -EIO : 0;
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(power, "power", NULL, power_setting_set, NULL, NULL);

/* PCA9420 charger states in which the cell is as full as it gets. */
#define CHARGER_TOP_OFF 5
#define CHARGER_DONE    6

static int charge(struct power_status *st)
{
	static uint16_t saved_raw;
	int mix = gauge_read(GAUGE_MIX_CAP);
	int rep_percent = gauge_read(GAUGE_REP_SOC) >> 8;
	int full = full_raw;

	if (mix == 0) {
		return -EIO;
	}
	if (saved_raw == 0) {
		saved_raw = full_raw;
	}
	if (st->usb_power &&
	    (st->charger_state == CHARGER_TOP_OFF || st->charger_state == CHARGER_DONE)) {
		full = mix;
		full_raw = mix;
		/* Kept for a start on battery; not rewritten for a step or two. */
		if (abs(mix - saved_raw) >= 4) {
			saved_raw = mix;
			settings_save_one("power/full", &full_raw, sizeof(full_raw));
		}
	} else if (full < mix) {
		/* Fuller than full was thought to be, or full never seen. */
		full = mix;
		if (full_raw != 0) {
			full_raw = mix;
		}
	}
	st->percent = CLAMP(MIN((mix * 100 + full / 2) / full, rep_percent), 0, 100);
	/* The gauge counts for a 5 mOhm sense resistor; the watch has 10 mOhm. */
	st->remaining_mah = mix / 2;
	st->full_mah = full / 2;
	st->gauge_percent = rep_percent;
	return 0;
}

int power_status(struct power_status *st)
{
	struct sensor_value v;
	uint8_t reg;
	int ret;

	ret = sensor_sample_fetch(gauge);
	if (ret < 0) {
		return ret;
	}
	sensor_channel_get(gauge, SENSOR_CHAN_GAUGE_VOLTAGE, &v);
	st->millivolts = milli(&v);
	sensor_channel_get(gauge, SENSOR_CHAN_GAUGE_AVG_CURRENT, &v);
	/* The driver reports mA with the remainder in nA. */
	st->microamps = v.val1 * 1000 + v.val2 / 1000;
	ret = i2c_reg_read_byte(i2c, PMIC_ADDR, PMIC_CHG_STATUS_0, &reg);
	if (ret < 0) {
		return ret;
	}
	st->usb_power = (reg & CHG_STATUS_0_VIN_OK) != 0;

	ret = i2c_reg_read_byte(i2c, PMIC_ADDR, PMIC_CHG_STATUS_2, &reg);
	if (ret < 0) {
		return ret;
	}
	st->charger_state = reg & CHG_STATUS_2_STATE;

	ret = i2c_reg_read_byte(i2c, PMIC_ADDR, PMIC_CHG_CNTL1, &reg);
	if (ret < 0) {
		return ret;
	}
	st->charge_limit_ma = (reg & 0x3f) * 5;

	return charge(st);
}

static struct power_status last_status;
static struct k_spinlock last_lock;

void power_last(struct power_status *st)
{
	K_SPINLOCK(&last_lock) {
		*st = last_status;
	}
}

int power_off(void)
{
	/* With USB power present the PMIC starts again at once. */
	return i2c_reg_update_byte(i2c, PMIC_ADDR, PMIC_TOP_CNTL3, TOP_CNTL3_MODE_MASK,
				   TOP_CNTL3_MODE_OFF);
}

/*
 * A sample every POLL_S seconds, kept in RAM. On battery there is no console,
 * so this is how the current drawn while asleep is read afterwards.
 */
#define POLL_S    5
#define HISTORY_N 720 /* one hour */

/* How often the SoC has entered a Stop state. */
static uint32_t stop_entries;

static void on_pm_state_entry(enum pm_state state)
{
	if (state == PM_STATE_SUSPEND_TO_IDLE) {
		stop_entries++;
	}
}

static struct pm_notifier pm_notifier = {.state_entry = on_pm_state_entry};

struct sample {
	uint32_t stop_entries;
	bool stop_locked;
	uint32_t uptime_s;
	int32_t microamps;
	uint16_t millivolts;
	uint8_t percent;
	bool usb_power;
};

static struct sample history[HISTORY_N];
static uint32_t history_count;

/* Of the last whole minute; see telemetry(). */
static uint32_t minute_awake_permille;
static uint32_t minute_stops;

#define PER_MINUTE (60 / POLL_S)

int power_trend(int32_t *microamps)
{
	uint32_t count = history_count;
	int minutes = MIN(count, HISTORY_N) / PER_MINUTE;

	for (int m = 0; m < minutes; m++) {
		uint32_t first = count - (minutes - m) * PER_MINUTE;
		int64_t sum = 0;

		for (int i = 0; i < PER_MINUTE; i++) {
			sum += history[(first + i) % HISTORY_N].microamps;
		}
		microamps[m] = sum / PER_MINUTE;
	}
	return minutes;
}

void power_stats(struct power_stats *stats)
{
	uint32_t count = history_count;
	uint32_t n = MIN(count, HISTORY_N);
	int64_t sum = 0;

	for (uint32_t i = count - n; i < count; i++) {
		sum += history[i % HISTORY_N].microamps;
	}
	stats->mean_ua = n > 0 ? sum / n : 0;
	stats->awake_permille = minute_awake_permille;
	stats->stops_per_min = minute_stops;
	stats->stop_locked = pm_policy_state_lock_is_active(PM_STATE_SUSPEND_TO_IDLE, 3);
}

/*
 * One line a minute through the ordinary log. The file-system log backend
 * keeps it on the eMMC (/USER:/Log/Zephyr), which is what makes battery life
 * and usage readable after the fact: on battery there is no console.
 */
#define TELEMETRY_EVERY (60 / POLL_S)

static void telemetry(const struct power_status *st, int32_t mean_ua)
{
	static const struct device *const rtc = DEVICE_DT_GET(DT_ALIAS(rtc));
	static uint32_t last_stop_entries;
	static uint64_t last_busy, last_idle;
	k_thread_runtime_stats_t cpu;
	struct rtc_time t = {0};
	uint32_t awake_permille = 0;

	if (k_thread_runtime_stats_cpu_get(0, &cpu) == 0) {
		uint64_t busy = cpu.total_cycles - last_busy;
		uint64_t idle = cpu.idle_cycles - last_idle;

		if (busy + idle > 0) {
			awake_permille = (uint32_t)(busy * 1000 / (busy + idle));
		}
		last_busy = cpu.total_cycles;
		last_idle = cpu.idle_cycles;
	}
	(void)rtc_get_time(rtc, &t);

	/*
	 * soc is the percentage shown, cap the charge it is made from; rep is
	 * the gauge's own percentage and qh its plain coulomb counter (0.5 mAh
	 * a step), to check the first against. See charge().
	 */
	LOG_INF("tm %04d-%02d-%02dT%02d:%02d:%02dZ usb=%d soc=%d cap=%d/%d rep=%d qh=%d mv=%d ua=%d "
		"chg=%u stops=%u awake=%u/1000 keys=%ld backlight_ms=%ld",
		t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
		st->usb_power, st->percent, st->remaining_mah, st->full_mah, st->gauge_percent,
		gauge_qh(),
		st->millivolts, mean_ua, st->charger_state,
		stop_entries - last_stop_entries, awake_permille,
		(long)atomic_clear(&usage_key_presses), (long)atomic_clear(&usage_backlight_ms));
	minute_awake_permille = awake_permille;
	minute_stops = stop_entries - last_stop_entries;
	last_stop_entries = stop_entries;
}

/*
 * The log core tells backends that a batch is done, which is when the file
 * backend syncs, only after two or more messages in a row. A lone line a
 * minute would stay in the file system's buffers until the next burst, and be
 * lost on a reset. So: wait for the log thread to have written it, then send
 * the same notification.
 */
static void log_file_sync(void)
{
	const struct log_backend *fs = log_backend_get_by_name("log_backend_fs");

	if (fs != NULL && log_backend_is_active(fs)) {
		k_sleep(K_MSEC(1500));
		log_backend_notify(fs, LOG_BACKEND_EVT_PROCESS_THREAD_DONE, NULL);
	}
}

static void power_thread(void *p1, void *p2, void *p3)
{
	struct power_status st;
	bool usb_power = true;
	int64_t sum_ua = 0;
	int polls = 0;

	pm_notifier_register(&pm_notifier);

	while (true) {
		if (power_status(&st) == 0) {
			K_SPINLOCK(&last_lock) {
				last_status = st;
			}
			if (st.usb_power != usb_power) {
				usb_power = st.usb_power;
				LOG_INF("usb power %s, soc=%d mv=%d", usb_power ? "on" : "off",
					st.percent, st.millivolts);
				log_file_sync();
			}
			sum_ua += st.microamps;
			if (++polls == TELEMETRY_EVERY) {
				telemetry(&st, (int32_t)(sum_ua / polls));
				log_file_sync();
				sum_ua = 0;
				polls = 0;
			}

			struct sample *s = &history[history_count++ % HISTORY_N];

			s->uptime_s = k_uptime_get() / 1000;
			s->microamps = st.microamps;
			s->millivolts = st.millivolts;
			s->percent = st.percent;
			s->usb_power = st.usb_power;
			s->stop_entries = stop_entries;
			s->stop_locked = pm_policy_state_lock_is_active(PM_STATE_SUSPEND_TO_IDLE, 3);

			usb_set_enabled(st.usb_power);
		}
		k_sleep(K_SECONDS(POLL_S));
	}
}
K_THREAD_DEFINE(power_tid, 4096, power_thread, NULL, NULL, NULL, 8, 0, 2000);

static int cmd_history(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n = MIN(history_count, HISTORY_N);

	shell_print(sh, "uptime_s usb mV %% uA stop_locked stop_entries");
	for (uint32_t i = history_count - n; i < history_count; i++) {
		const struct sample *s = &history[i % HISTORY_N];

		shell_print(sh, "%u %d %u %u %d %d %u", s->uptime_s, s->usb_power, s->millivolts,
			    s->percent, s->microamps, s->stop_locked, s->stop_entries);
	}
	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct power_status st;
	int ret = power_status(&st);

	if (ret < 0) {
		shell_error(sh, "read failed: %d", ret);
		return ret;
	}
	shell_print(sh, "battery: %d %% (gauge: %d %%), %d mV, %d uA, %d of %d mAh", st.percent,
		    st.gauge_percent, st.millivolts, st.microamps, st.remaining_mah, st.full_mah);
	shell_print(sh, "usb power: %s, charger state %u, limit %u mA",
		    st.usb_power ? "yes" : "no", st.charger_state, st.charge_limit_ma);
	return 0;
}

static int cmd_off(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "powering off");
	k_msleep(100);
	return power_off();
}

/* Exercises the on-battery USB teardown and restart without unplugging. */
static int cmd_usbcycle(const struct shell *sh, size_t argc, char **argv)
{
	usb_set_enabled(false);
	k_sleep(K_SECONDS(3));
	usb_set_enabled(true);
	return 0;
}

/*
 * With USB off for 3 s: is anything else keeping the SoC out of Stop mode?
 * The console is gone meanwhile, so the answer is kept for "pmdiag show".
 */
static char diag[101];

static int cmd_pmdiag(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		shell_print(sh, "%s", diag);
		return 0;
	}
	usb_set_enabled(false);
	for (int i = 0; i < 100; i++) {
		k_sleep(K_MSEC(100));
		diag[i] = pm_policy_state_lock_is_active(PM_STATE_SUSPEND_TO_IDLE, 3) ? 'L' : '.';
	}
	usb_set_enabled(true);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(power_cmds,
	SHELL_CMD(status, NULL, "Battery and charger state", cmd_status),
	SHELL_CMD(history, NULL, "Samples of the last hour, one every 5 s", cmd_history),
	SHELL_CMD(usbcycle, NULL, "Stop USB for 3 s and start it again", cmd_usbcycle),
	SHELL_CMD(pmdiag, NULL, "Stop USB for 10 s and note Stop-mode locks; \"pmdiag show\" prints them", cmd_pmdiag),
	SHELL_CMD(off, NULL, "Power the watch off (wake with the power button)", cmd_off),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(power, &power_cmds, "Battery, charger, power-off", NULL);
