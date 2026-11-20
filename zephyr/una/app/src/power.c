/*
 * Battery, charger and power-off.
 *
 * The PCA9420 is driven register by register rather than through Zephyr's
 * regulator driver: the UNA bootloader has already set the four rails the
 * watch runs on, and nothing here should rewrite them. Register layout as in
 * Libs/Header/Hardware/PCA9420.hpp of una-kernel-public.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

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

int power_status(struct power_status *st)
{
	struct sensor_value v;
	uint8_t reg;
	int ret;

	ret = sensor_sample_fetch(gauge);
	if (ret < 0) {
		return ret;
	}
	sensor_channel_get(gauge, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE, &v);
	st->percent = CLAMP(v.val1, 0, 100);
	sensor_channel_get(gauge, SENSOR_CHAN_GAUGE_VOLTAGE, &v);
	st->millivolts = milli(&v);
	sensor_channel_get(gauge, SENSOR_CHAN_GAUGE_AVG_CURRENT, &v);
	/* The driver reports mA with the remainder in nA. */
	st->microamps = v.val1 * 1000 + v.val2 / 1000;
	sensor_channel_get(gauge, SENSOR_CHAN_GAUGE_REMAINING_CHARGE_CAPACITY, &v);
	/* The driver assumes a 5 mOhm sense resistor; the watch has 10 mOhm. */
	st->remaining_mah = milli(&v) / 2;
	sensor_channel_get(gauge, SENSOR_CHAN_GAUGE_FULL_CHARGE_CAPACITY, &v);
	st->full_mah = milli(&v) / 2;

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

	return 0;
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

struct sample {
	uint32_t uptime_s;
	int32_t microamps;
	uint16_t millivolts;
	uint8_t percent;
	bool usb_power;
};

static struct sample history[HISTORY_N];
static uint32_t history_count;

static void power_thread(void *p1, void *p2, void *p3)
{
	struct power_status st;

	while (true) {
		if (power_status(&st) == 0) {
			struct sample *s = &history[history_count++ % HISTORY_N];

			s->uptime_s = k_uptime_get() / 1000;
			s->microamps = st.microamps;
			s->millivolts = st.millivolts;
			s->percent = st.percent;
			s->usb_power = st.usb_power;

			usb_set_enabled(st.usb_power);
		}
		k_sleep(K_SECONDS(POLL_S));
	}
}
K_THREAD_DEFINE(power_tid, 2048, power_thread, NULL, NULL, NULL, 8, 0, 10000);

static int cmd_history(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n = MIN(history_count, HISTORY_N);

	shell_print(sh, "uptime_s usb mV %% uA");
	for (uint32_t i = history_count - n; i < history_count; i++) {
		const struct sample *s = &history[i % HISTORY_N];

		shell_print(sh, "%u %d %u %u %d", s->uptime_s, s->usb_power, s->millivolts,
			    s->percent, s->microamps);
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
	shell_print(sh, "battery: %d %%, %d mV, %d uA, %d of %d mAh", st.percent, st.millivolts,
		    st.microamps, st.remaining_mah, st.full_mah);
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

SHELL_STATIC_SUBCMD_SET_CREATE(power_cmds,
	SHELL_CMD(status, NULL, "Battery and charger state", cmd_status),
	SHELL_CMD(history, NULL, "Samples of the last hour, one every 5 s", cmd_history),
	SHELL_CMD(off, NULL, "Power the watch off (wake with the power button)", cmd_off),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(power, &power_cmds, "Battery, charger, power-off", NULL);
