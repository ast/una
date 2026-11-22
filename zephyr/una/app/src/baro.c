/*
 * The barometer, for height gained and lost: powered while anyone wants it,
 * read once a second, the readings evened out.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include "baro.h"
#include "ui.h"

LOG_MODULE_REGISTER(baro, LOG_LEVEL_INF);

#define PERIOD_MS 1000
/* A reading older than this is not where the watch is now. */
#define FRESH_MS  3000
/*
 * Each reading moves the height a quarter of the way: a wrist swung and a
 * gust are gone in it, a stair climbed shows within a few seconds.
 */
#define SMOOTH    4
/* So many readings failed in a row: the supply is taken away and given back. */
#define FAILURES_TO_RESTART 5

static atomic_t wanted;
static K_SEM_DEFINE(wake, 0, 1);

static struct k_spinlock lock;
static struct baro_reading last;
static bool have_last;
static struct baro_status state;

void baro_want(enum baro_user user, bool on)
{
	if (on) {
		atomic_set_bit(&wanted, user);
	} else {
		atomic_clear_bit(&wanted, user);
	}
	k_sem_give(&wake);
}

bool baro_wanted(enum baro_user user)
{
	return atomic_test_bit(&wanted, user);
}

bool baro_read(struct baro_reading *reading)
{
	bool fresh;

	K_SPINLOCK(&lock) {
		*reading = last;
		fresh = have_last && k_uptime_get() - last.at_ms < FRESH_MS;
	}
	return fresh;
}

void baro_status(struct baro_status *status)
{
	K_SPINLOCK(&lock) {
		*status = state;
	}
}

/*
 * The standard atmosphere: 1013.25 hPa at the sea, and from there
 * 44330.77 m * (1 - (p / p0) ^ 0.190263). The C library here has no
 * mathematics; the power is made of a logarithm and an exponential, both
 * from their series, which are short over the pressures there are.
 */
static int32_t altitude_of(int32_t pressure_pa)
{
	double x = pressure_pa / 101325.0;
	double z = (x - 1) / (x + 1), z2 = z * z;
	double term = z, ln = 0, y, power = 1;

	for (int n = 1; n < 40; n += 2) {
		ln += term / n;
		term *= z2;
	}
	y = 0.190263 * 2 * ln;
	term = 1;
	for (int n = 1; n < 16; n++) {
		term *= y / n;
		power += term;
	}
	return (int32_t)(44330770.0 * (1.0 - power));
}

static bool set_power(bool on)
{
	int ret = baro_hw_power(on);

	K_SPINLOCK(&lock) {
		state.on = on && ret == 0;
		state.readings = 0;
		have_last = false;
		if (on) {
			state.present = ret == 0;
		}
	}
	if (on && ret != 0) {
		LOG_ERR("no sensor: %d", ret);
		(void)baro_hw_power(false);
		/* Whoever wanted it must ask again. */
		atomic_clear(&wanted);
	} else {
		LOG_INF("sensor %s (wanted by %lx)", on ? "on" : "off", (long)atomic_get(&wanted));
	}
	ui_refresh();
	return on && ret == 0;
}

static void sample(void)
{
	static int in_a_row;
	int32_t pressure_pa, temperature_cc, altitude_mm;
	int ret = baro_hw_sample(&pressure_pa, &temperature_cc);

	/* Outside what the sensor measures is no reading either. */
	if (ret == 0 && (pressure_pa < 30000 || pressure_pa > 120000)) {
		ret = -ERANGE;
	}
	if (ret != 0) {
		K_SPINLOCK(&lock) {
			state.failures++;
		}
		if (++in_a_row == FAILURES_TO_RESTART) {
			LOG_WRN("reading fails (%d): supply off and on", ret);
			in_a_row = 0;
			(void)baro_hw_power(false);
			k_msleep(20);
			if (!set_power(true)) {
				return;
			}
		}
		return;
	}
	in_a_row = 0;
	altitude_mm = altitude_of(pressure_pa);
	K_SPINLOCK(&lock) {
		if (have_last) {
			altitude_mm = last.altitude_mm + (altitude_mm - last.altitude_mm) / SMOOTH;
		}
		last = (struct baro_reading){
			.pressure_pa = pressure_pa,
			.temperature_cc = temperature_cc,
			.altitude_mm = altitude_mm,
			.at_ms = k_uptime_get(),
		};
		have_last = true;
		state.readings++;
	}
}

static void baro_thread(void *p1, void *p2, void *p3)
{
	bool on = false;
	int64_t next_ms = 0;

	(void)baro_hw_power(false);
	while (true) {
		bool want = atomic_get(&wanted) != 0;

		if (want != on) {
			on = set_power(want);
			next_ms = k_uptime_get();
		}
		if (!on) {
			k_sem_take(&wake, K_FOREVER);
			continue;
		}
		if (k_uptime_get() >= next_ms) {
			sample();
			on = state.on;
			next_ms += PERIOD_MS;
			if (next_ms < k_uptime_get()) {
				next_ms = k_uptime_get() + PERIOD_MS;
			}
		}
		k_sem_take(&wake, K_TIMEOUT_ABS_MS(next_ms));
	}
}
K_THREAD_DEFINE(baro_tid, 2048, baro_thread, NULL, NULL, NULL, 9, 0, 500);

/* baro [on|off] */
static int cmd_baro(const struct shell *sh, size_t argc, char **argv)
{
	struct baro_reading r;
	struct baro_status s;

	if (argc > 1) {
		baro_want(BARO_FOR_SHELL, strcmp(argv[1], "on") == 0);
		return 0;
	}
	baro_status(&s);
	shell_print(sh, "%s, %s, %u readings, %u failures",
		    s.present ? "present" : "not seen", s.on ? "on" : "off", s.readings,
		    s.failures);
	if (baro_read(&r)) {
		shell_print(sh, "%d Pa, %d.%02d C, %d mm in the standard atmosphere",
			    (int)r.pressure_pa, (int)(r.temperature_cc / 100),
			    abs((int)(r.temperature_cc % 100)), (int)r.altitude_mm);
	}
	return 0;
}
SHELL_CMD_ARG_REGISTER(baro, NULL, "Barometer state, or turn it on or off: baro [on|off]",
		       cmd_baro, 1, 1);
