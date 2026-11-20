/*
 * Simulator (native_sim) stand-ins for the watch's hardware: what the UI
 * asks of src/power.c, answered with values set from the shell.
 */

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "alert.h"
#include "power.h"
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
	ui_refresh();
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
	SHELL_CMD(state, NULL, "What the UI has asked of the hardware", cmd_state),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(sim, &sim_cmds, "Simulated hardware", NULL);
