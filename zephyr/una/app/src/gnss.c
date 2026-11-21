/*
 * The GNSS receiver: see gnss.h.
 *
 * A thread of its own turns the receiver on and off, which takes seconds,
 * and while it is on writes a line to the log every ten seconds: what the
 * receiver hears is only found out outdoors, away from a console, and read
 * from the log file afterwards.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gnss.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>
#include <zephyr/shell/shell.h>

#include "gnss.h"
#include "ui.h"

LOG_MODULE_REGISTER(gnss, LOG_LEVEL_INF);

/* A fix older than this is no fix. */
#define FRESH_MS 3000
#define LOG_EVERY_S 10

#define GNSS_NODE DT_ALIAS(gnss)

#if DT_NODE_EXISTS(GNSS_NODE)

static const struct device *const receiver = DEVICE_DT_GET(GNSS_NODE);

#if DT_NODE_HAS_COMPAT(GNSS_NODE, airoha_ag3335m)
#include <una/gnss_ag3335m.h>

static int power(bool on)
{
	return gnss_ag3335m_power(receiver, on);
}

static uint32_t sentences(void)
{
	return gnss_ag3335m_sentences(receiver);
}
#else
/* The simulator's emulated receiver. */
static int power(bool on)
{
	int ret = pm_device_action_run(receiver,
				       on ? PM_DEVICE_ACTION_RESUME : PM_DEVICE_ACTION_SUSPEND);

	return ret == -EALREADY ? 0 : ret;
}

static uint32_t sentences(void)
{
	return 0;
}
#endif

static atomic_t wanted;
static K_SEM_DEFINE(changed, 0, 1);

static struct k_spinlock lock;
static struct gnss_fix last;
static bool last_valid;
static struct gnss_status state = {.present = true, .first_fix_s = -1};
static int64_t on_at_ms;

void gnss_want(enum gnss_user user, bool on)
{
	if (on) {
		atomic_set_bit(&wanted, user);
	} else {
		atomic_clear_bit(&wanted, user);
	}
	k_sem_give(&changed);
}

bool gnss_wanted(enum gnss_user user)
{
	return atomic_test_bit(&wanted, user);
}

bool gnss_fix(struct gnss_fix *fix)
{
	bool fresh;

	K_SPINLOCK(&lock) {
		*fix = last;
		fresh = last_valid && k_uptime_get() - last.at_ms < FRESH_MS;
	}
	return fresh;
}

void gnss_status(struct gnss_status *status)
{
	K_SPINLOCK(&lock) {
		*status = state;
		status->fix = last_valid && k_uptime_get() - last.at_ms < FRESH_MS;
		status->on_s = state.on ? (k_uptime_get() - on_at_ms) / 1000 : 0;
	}
	status->sentences = status->on ? sentences() : 0;
}

static void on_data(const struct device *dev, const struct gnss_data *data)
{
	bool valid = data->info.fix_status != GNSS_FIX_STATUS_NO_FIX;
	bool first = false;

	ARG_UNUSED(dev);

	K_SPINLOCK(&lock) {
		last_valid = valid;
		if (valid) {
			last.latitude = data->nav_data.latitude;
			last.longitude = data->nav_data.longitude;
			last.altitude_mm = data->nav_data.altitude;
			last.speed_mm_s = data->nav_data.speed;
			last.at_ms = k_uptime_get();
			if (state.first_fix_s < 0) {
				state.first_fix_s = (last.at_ms - on_at_ms) / 1000;
				first = true;
			}
		}
		state.used = data->info.satellites_cnt;
		state.hdop_tenths = data->info.hdop / 100;
	}
	if (first) {
		LOG_INF("first fix after %d s: %lld %lld nanodegrees, %d mm", state.first_fix_s,
			data->nav_data.latitude, data->nav_data.longitude,
			data->nav_data.altitude);
	}
}
GNSS_DATA_CALLBACK_DEFINE(DEVICE_DT_GET(GNSS_NODE), on_data);

#ifdef CONFIG_GNSS_SATELLITES
static void on_satellites(const struct device *dev, const struct gnss_satellite *satellites,
			  uint16_t size)
{
	uint8_t snr[GNSS_SNR_N] = {0};
	int seen = 0;

	ARG_UNUSED(dev);

	/* The strongest, strongest first. */
	for (int i = 0; i < size; i++) {
		uint8_t s = satellites[i].snr;

		seen += s > 0;
		for (int j = 0; j < GNSS_SNR_N && s > 0; j++) {
			if (s > snr[j]) {
				uint8_t lower = snr[j];

				snr[j] = s;
				s = lower;
			}
		}
	}
	K_SPINLOCK(&lock) {
		state.seen = seen;
		memcpy(state.snr, snr, sizeof(snr));
	}
}
GNSS_SATELLITES_CALLBACK_DEFINE(DEVICE_DT_GET(GNSS_NODE), on_satellites);
#endif

static void stop_lock(bool hold)
{
#ifdef CONFIG_PM
	if (hold) {
		pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
	} else {
		pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
	}
#else
	ARG_UNUSED(hold);
#endif
}

static void set_power(bool on)
{
	int ret;

	/*
	 * The UART's clock stops in Stop mode: the SoC stays out of it from
	 * before the first command to the receiver until it is off.
	 */
	if (on) {
		stop_lock(true);
	}
	ret = power(on);
	if (ret < 0) {
		LOG_ERR("receiver not turned %s: %d", on ? "on" : "off", ret);
		if (on) {
			stop_lock(false);
			/* Whoever wanted it must ask again. */
			atomic_clear(&wanted);
			return;
		}
	}
	if (!on) {
		stop_lock(false);
	}
	K_SPINLOCK(&lock) {
		state.on = on;
		state.first_fix_s = -1;
		state.used = 0;
		state.seen = 0;
		memset(state.snr, 0, sizeof(state.snr));
		last_valid = false;
		on_at_ms = k_uptime_get();
	}
	LOG_INF("receiver %s (wanted by %lx)", on ? "on" : "off", (long)atomic_get(&wanted));
	ui_refresh();
}

static void log_status(void)
{
	struct gnss_status s;
	struct gnss_fix fix;
	bool fresh = gnss_fix(&fix);

	gnss_status(&s);
	LOG_INF("on %u s, fix %d, used %d, seen %d, snr %u %u %u %u %u %u, hdop %d.%d, "
		"sentences %u, first fix %d s",
		s.on_s, fresh, s.used, s.seen, s.snr[0], s.snr[1], s.snr[2], s.snr[3], s.snr[4],
		s.snr[5], s.hdop_tenths / 10, s.hdop_tenths % 10, s.sentences, s.first_fix_s);
	if (fresh) {
		LOG_INF("at %lld %lld nanodegrees, %d mm, %u mm/s", fix.latitude, fix.longitude,
			fix.altitude_mm, fix.speed_mm_s);
	}
}

static void gnss_thread(void *p1, void *p2, void *p3)
{
	bool on = false;

	if (!device_is_ready(receiver)) {
		LOG_ERR("receiver not ready");
		state.present = false;
		return;
	}
	/* The emulated one starts on. */
	(void)power(false);

	while (true) {
		bool want;

		if (k_sem_take(&changed, on ? K_SECONDS(LOG_EVERY_S) : K_FOREVER) != 0) {
			log_status();
			continue;
		}
		want = atomic_get(&wanted) != 0;
		if (want != on) {
			set_power(want);
			on = state.on;
		}
	}
}
K_THREAD_DEFINE(gnss_tid, 3072, gnss_thread, NULL, NULL, NULL, 9, 0, 500);

/* gnss [on|off] */
static int cmd_gnss(const struct shell *sh, size_t argc, char **argv)
{
	struct gnss_status s;
	struct gnss_fix fix;
	bool fresh;

	if (argc > 1) {
		gnss_want(GNSS_FOR_SHELL, strcmp(argv[1], "on") == 0);
		return 0;
	}
	fresh = gnss_fix(&fix);
	gnss_status(&s);
	shell_print(sh, "%s %u s, fix %d (first after %d s), used %d, seen %d, hdop %d.%d, "
		    "%u sentences",
		    s.on ? "on" : "off", s.on_s, fresh, s.first_fix_s, s.used, s.seen,
		    s.hdop_tenths / 10, s.hdop_tenths % 10, s.sentences);
	shell_print(sh, "snr %u %u %u %u %u %u %u %u %u %u %u %u", s.snr[0], s.snr[1], s.snr[2],
		    s.snr[3], s.snr[4], s.snr[5], s.snr[6], s.snr[7], s.snr[8], s.snr[9],
		    s.snr[10], s.snr[11]);
	if (fresh) {
		shell_print(sh, "at %lld %lld nanodegrees, %d mm, %u mm/s", fix.latitude,
			    fix.longitude, fix.altitude_mm, fix.speed_mm_s);
	}
	return 0;
}
SHELL_CMD_ARG_REGISTER(gnss, NULL, "The GNSS receiver: gnss [on|off]", cmd_gnss, 1, 1);

#else /* no receiver */

void gnss_want(enum gnss_user user, bool on)
{
	ARG_UNUSED(user);
	ARG_UNUSED(on);
}

bool gnss_wanted(enum gnss_user user)
{
	ARG_UNUSED(user);

	return false;
}

bool gnss_fix(struct gnss_fix *fix)
{
	ARG_UNUSED(fix);

	return false;
}

void gnss_status(struct gnss_status *status)
{
	*status = (struct gnss_status){.first_fix_s = -1};
}

#endif
