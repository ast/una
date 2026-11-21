/*
 * Simulator (native_sim) stand-ins for the watch's hardware: what the UI
 * asks of src/power.c, answered with values set from the shell.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/drivers/gnss/gnss_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "alert.h"
#include "power.h"
#include "storage.h"
#include "ui.h"

LOG_MODULE_REGISTER(sim, LOG_LEVEL_INF);

static struct power_status status = {
	.percent = 80,
	.millivolts = 4000,
	.microamps = -1000,
	.remaining_mah = 160,
	.full_mah = 200,
};
static bool powered_off;
static bool buzzer_on, motor_on;
static unsigned int buzzer_pulses, motor_pulses;

void buzzer_set(bool on)
{
	buzzer_pulses += on && !buzzer_on;
	buzzer_on = on;
}

void motor_set(bool on)
{
	motor_pulses += on && !motor_on;
	motor_on = on;
}

int power_status(struct power_status *st)
{
	*st = status;
	return 0;
}

void power_last(struct power_status *st)
{
	*st = status;
}

void power_stats(struct power_stats *stats)
{
	*stats = (struct power_stats){
		.mean_ua = status.usb_power ? 30000 : -1010,
		.awake_permille = 1,
		.stops_per_min = status.usb_power ? 0 : 134,
		.stop_locked = status.usb_power,
	};
}

/* An hour that began on the charger. */
int power_trend(int32_t *microamps)
{
	for (int i = 0; i < POWER_TREND_N; i++) {
		microamps[i] = i < 15 ? 40000 - i * 2000 : -800 - (i * 37 % 11) * 60;
	}
	return POWER_TREND_N;
}

bool storage_info(struct storage_info *info, bool measure)
{
	ARG_UNUSED(measure);

	*info = (struct storage_info){.total_mb = 7400, .free_mb = 5100};
	return true;
}

int power_off(void)
{
	LOG_INF("power off");
	powered_off = true;
	return 0;
}

/* sim battery <percent> <usb power 0|1> [charger state 0-6] */
static int cmd_battery(const struct shell *sh, size_t argc, char **argv)
{
	status.percent = atoi(argv[1]);
	status.usb_power = atoi(argv[2]) != 0;
	status.charger_state = argc > 3 ? atoi(argv[3]) : 0;
	status.remaining_mah = status.full_mah * status.percent / 100;
	status.millivolts = 3400 + 10 * status.percent;
	status.microamps = status.usb_power ? 50000 : -1000;
	status.charge_limit_ma = 100;
	ui_refresh();
	return 0;
}

/* The receiver: where it is, and how it moves. */
static const struct device *const gnss = DEVICE_DT_GET(DT_ALIAS(gnss));
static struct navigation_data nav;
static int climb_mm_s;

static void gnss_set(bool fix)
{
	struct gnss_info info = {
		.satellites_cnt = fix ? 9 : 0,
		.hdop = fix ? 900 : 99000,
		.fix_status = fix ? GNSS_FIX_STATUS_GNSS_FIX : GNSS_FIX_STATUS_NO_FIX,
		.fix_quality = fix ? GNSS_FIX_QUALITY_GNSS_SPS : GNSS_FIX_QUALITY_INVALID,
	};

	gnss_emul_set_data(gnss, &nav, &info, 0);
}

static void move(struct k_work *work)
{
	const double deg_per_m = 1e9 / 111195.0; /* nanodegrees */
	double heading = nav.bearing / 1000.0 * 3.14159265358979323846 / 180;
	double m = nav.speed / 1000.0;

	nav.latitude += (int64_t)(m * cos(heading) * deg_per_m);
	nav.longitude += (int64_t)(m * sin(heading) * deg_per_m /
				   cos(nav.latitude / 1e9 * 3.14159265358979323846 / 180));
	nav.altitude += climb_mm_s;
	gnss_set(true);
	if (nav.speed > 0) {
		k_work_schedule(k_work_delayable_from_work(work), K_SECONDS(1));
	}
}
static K_WORK_DELAYABLE_DEFINE(move_work, move);

/* sim gps off | <latitude> <longitude> [altitude m] */
static int cmd_gps(const struct shell *sh, size_t argc, char **argv)
{
	if (strcmp(argv[1], "off") == 0) {
		nav.speed = 0;
		gnss_set(false);
		return 0;
	}
	if (argc < 3) {
		return -EINVAL;
	}
	nav.latitude = (int64_t)(strtod(argv[1], NULL) * 1e9);
	nav.longitude = (int64_t)(strtod(argv[2], NULL) * 1e9);
	nav.altitude = argc > 3 ? atoi(argv[3]) * 1000 : 0;
	gnss_set(true);
	return 0;
}

/* sim move <km/h> <heading> [climb m/min] */
static int cmd_move(const struct shell *sh, size_t argc, char **argv)
{
	nav.speed = (uint32_t)(strtod(argv[1], NULL) * 1000000 / 3600);
	nav.bearing = atoi(argv[2]) * 1000;
	climb_mm_s = argc > 3 ? atoi(argv[3]) * 1000 / 60 : 0;
	k_work_schedule(&move_work, K_NO_WAIT);
	return 0;
}

static int cmd_state(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "power off asked for: %s", powered_off ? "yes" : "no");
	shell_print(sh, "buzzer: %s, %u pulses so far", buzzer_on ? "on" : "off", buzzer_pulses);
	shell_print(sh, "motor: %s, %u pulses so far", motor_on ? "on" : "off", motor_pulses);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sim_cmds,
	SHELL_CMD_ARG(battery, NULL, "<percent> <usb power 0|1> [charger state 0-6]", cmd_battery,
		      3, 1),
	SHELL_CMD_ARG(gps, NULL, "off | <latitude> <longitude> [altitude m]", cmd_gps, 2, 2),
	SHELL_CMD_ARG(move, NULL, "<km/h> <heading> [climb m/min]: move from where gps put it",
		      cmd_move, 3, 1),
	SHELL_CMD(state, NULL, "What the UI has asked of the hardware", cmd_state),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(sim, &sim_cmds, "Simulated hardware", NULL);
