/*
 * Simulator (native_sim) stand-ins for the watch's hardware: what the UI
 * asks of src/power.c, answered with values set from the shell.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/drivers/gnss/gnss_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "alert.h"
#include "baro.h"
#include "bluetooth.h"
#include "compass.h"
#include "hr.h"
#include "motion.h"
#include "pairing.h"
#include "phone.h"
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

/*
 * The barometer is where the receiver is, in weather that makes it read
 * some tens of metres off, and with a little noise.
 */
#define BARO_WEATHER_MM 35000

static bool baro_there = true;
static bool baro_powered;

int baro_hw_power(bool on)
{
	baro_powered = on && baro_there;
	return on && !baro_there ? -EIO : 0;
}

int baro_hw_sample(int32_t *pressure_pa, int32_t *temperature_cc)
{
	static uint32_t noise = 1;
	double mm;

	if (!baro_powered || !baro_there) {
		return -EIO;
	}
	noise = noise * 1103515245 + 12345;
	mm = nav.altitude + BARO_WEATHER_MM + (int)(noise >> 16) % 300 - 150;
	*pressure_pa = (int32_t)(101325.0 * pow(1.0 - mm / 44330770.0, 1 / 0.190263));
	*temperature_cc = 2150;
	return 0;
}

/* sim baro on | off: there is a sensor, or none answers */
static int cmd_baro(const struct shell *sh, size_t argc, char **argv)
{
	baro_there = strcmp(argv[1], "on") == 0;
	return 0;
}

/*
 * The heart-rate sensor: skin under it, and light that dips with each beat
 * at the pulse the simulator is told, a hundredth of its level deep.
 */
static int hr_bpm = 64;
static bool hr_powered;
static int64_t hr_next_ms;

int hr_hw_power(bool on, bool always)
{
	ARG_UNUSED(always);

	hr_powered = on;
	hr_next_ms = k_uptime_get() + 1000;
	return 0;
}

int hr_hw_batch(struct hr_hw_batch *batch)
{
	static double phase;

	if (!hr_powered || k_uptime_get() < hr_next_ms) {
		return -ENODATA;
	}
	hr_next_ms += 1000;
	batch->worn = hr_bpm > 0;
	batch->count = 20;
	for (int i = 0; i < 20; i++) {
		uint32_t light = 12000000 + (uint32_t)(60000 * sin(phase));

		phase += 2 * 3.14159265 * hr_bpm / 60 / 20;
		batch->light[i][0] = light;
		batch->light[i][1] = light;
		batch->led[i] = 8;
	}
	return 0;
}

bool hr_hw_worn(void)
{
	return hr_powered && hr_bpm > 0;
}

void hr_hw_describe(char *text, size_t size)
{
	snprintf(text, size, "simulated, %d bpm", hr_bpm);
}

/* sim hr <bpm>: the pulse under the sensor; 0 takes the watch off */
static int cmd_hr(const struct shell *sh, size_t argc, char **argv)
{
	hr_bpm = atoi(argv[1]);
	return 0;
}

/*
 * The accelerometer: a watch that lies on its back, steps as the simulator
 * is told of them, and a wrist turned on command.
 */
static uint32_t steps;
static bool wrist_on;

int motion_hw_start(void)
{
	return 0;
}

int motion_hw_read(uint32_t *count, enum motion_activity *activity, int32_t accel_mg[3])
{
	static uint32_t last;

	*count = steps;
	*activity = steps != last ? MOTION_WALKING : MOTION_STILL;
	last = steps;
	return motion_hw_accel(accel_mg);
}

int motion_hw_accel(int32_t accel_mg[3])
{
	accel_mg[0] = 0;
	accel_mg[1] = 0;
	accel_mg[2] = 1000;
	return 0;
}

int motion_hw_wrist(bool on)
{
	wrist_on = on;
	return 0;
}

/* sim steps <more of them> */
static int cmd_steps(const struct shell *sh, size_t argc, char **argv)
{
	steps += atoi(argv[1]);
	motion_poll();
	return 0;
}

/* sim wrist: it is turned */
static int cmd_wrist(const struct shell *sh, size_t argc, char **argv)
{
	if (wrist_on) {
		motion_wrist_turned();
	}
	return 0;
}

/*
 * The magnetometer: the field of Stockholm, 16 uT of it level and 49 down,
 * about a watch that lies on its back with twelve o'clock where it is told.
 */
static int compass_deg;
static bool compass_powered;

int compass_hw_power(bool on)
{
	compass_powered = on;
	return 0;
}

int compass_hw_sample(int32_t field_nt[3])
{
	double heading = compass_deg * 3.14159265358979323846 / 180;

	if (!compass_powered) {
		return -EIO;
	}
	field_nt[0] = (int32_t)(-16000 * sin(heading));
	field_nt[1] = (int32_t)(16000 * cos(heading));
	field_nt[2] = -49000;
	return 0;
}

/* sim compass <degrees>: where twelve o'clock points */
static int cmd_compass(const struct shell *sh, size_t argc, char **argv)
{
	compass_deg = atoi(argv[1]);
	return 0;
}

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

/*
 * The radio: on or off as set, and a phone that is there when the simulator
 * is told so. What the watch sends it is logged.
 */

static bool radio_on = true, phone_there;
static uint32_t phone_rx_bytes, phone_tx_bytes;

void bluetooth_set(bool on)
{
	radio_on = on;
	phone_there = phone_there && on;
	ui_refresh();
}

bool bluetooth_is_set(void)
{
	return radio_on;
}

static uint8_t phones_paired = 1;

void bluetooth_forget(void)
{
	phones_paired = 0;
	phone_there = false;
	ui_refresh();
}

void bluetooth_status(struct bluetooth_status *status)
{
	*status = (struct bluetooth_status){
		.state = !radio_on     ? BLUETOOTH_OFF
			 : phone_there ? BLUETOOTH_CONNECTED
				       : BLUETOOTH_ADVERTISING,
		.secure = phone_there,
		.listening = phone_there,
		.bonds = phones_paired,
		.own = "C0:FF:EE:00:00:01",
		.interval_ms = 200,
		.latency = 4,
		.mtu = 131,
		.rx_bytes = phone_rx_bytes,
		.tx_bytes = phone_tx_bytes,
		.rssi = phone_there ? -60 : 0,
	};
}

bool bluetooth_send(const void *data, size_t len)
{
	if (!phone_there) {
		return false;
	}
	phone_tx_bytes += len;
	LOG_INF("to the phone: %.*s", (int)len, (const char *)data);
	return true;
}

/* sim pair <passkey> | off: the screen a phone that pairs brings up */
static void pair_cancelled(void)
{
	LOG_INF("pairing cancelled at the watch");
}

static int cmd_pair(const struct shell *sh, size_t argc, char **argv)
{
	if (strcmp(argv[1], "off") == 0) {
		pairing_hide();
	} else {
		pairing_show(strtoul(argv[1], NULL, 10), pair_cancelled);
	}
	return 0;
}

/* sim phone on | off | <a line it sends, in hex> */
static int cmd_phone(const struct shell *sh, size_t argc, char **argv)
{
	static uint8_t bytes[1500];
	size_t len;

	if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0) {
		phone_there = radio_on && argv[1][1] == 'n';
		if (phone_there) {
			phone_connected();
		} else {
			phone_disconnected();
		}
		ui_refresh();
		return 0;
	}
	len = hex2bin(argv[1], strlen(argv[1]), bytes, sizeof(bytes) - 1);
	if (len == 0 || !phone_there) {
		shell_error(sh, "no phone, or not hex");
		return -EINVAL;
	}
	bytes[len++] = '\n';
	phone_rx_bytes += len;
	phone_rx(bytes, len);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sim_cmds,
	SHELL_CMD_ARG(battery, NULL, "<percent> <usb power 0|1> [charger state 0-6]", cmd_battery,
		      3, 1),
	SHELL_CMD_ARG(baro, NULL, "on | off: the barometer answers, or not", cmd_baro, 2, 0),
	SHELL_CMD_ARG(compass, NULL, "<degrees>: where twelve o'clock points", cmd_compass, 2, 0),
	SHELL_CMD_ARG(gps, NULL, "off | <latitude> <longitude> [altitude m]", cmd_gps, 2, 2),
	SHELL_CMD_ARG(hr, NULL, "<bpm>: the pulse under the sensor; 0 takes the watch off", cmd_hr, 2,
		      0),
	SHELL_CMD_ARG(move, NULL, "<km/h> <heading> [climb m/min]: move from where gps put it",
		      cmd_move, 3, 1),
	SHELL_CMD_ARG(pair, NULL, "<passkey> | off: the pairing screen", cmd_pair, 2, 0),
	SHELL_CMD_ARG(phone, NULL, "on | off | <a line the phone sends, in hex>", cmd_phone, 2, 0),
	SHELL_CMD(state, NULL, "What the UI has asked of the hardware", cmd_state),
	SHELL_CMD_ARG(steps, NULL, "<count>: so many more steps are walked", cmd_steps, 2, 0),
	SHELL_CMD(wrist, NULL, "The wrist is turned for a look at the dial", cmd_wrist),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(sim, &sim_cmds, "Simulated hardware", NULL);
