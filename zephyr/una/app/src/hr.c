/*
 * The pulse: the sensor powered while anyone wants it, each second of its
 * samples put together with what the accelerometer read meanwhile and given
 * to the algorithm (src/hr_alg.c).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_BT_HRS)
#include <zephyr/bluetooth/services/hrs.h>
#endif

#include "clock.h"
#include "hr.h"
#include "hr_alg.h"
#include "motion.h"
#include "storage.h"
#include "ui.h"

LOG_MODULE_REGISTER(hr, LOG_LEVEL_INF);

/* A pulse older than this is not the wearer's now. */
#define FRESH_MS 5000
/* The accelerometer is read as often as the light is. */
#define ACCEL_MS (1000 / HR_ALG_HZ)
#define RAW_DIR  STORAGE_ROOT "/Log/PPG"

static atomic_t wanted;
static atomic_t always;
static K_SEM_DEFINE(wake, 0, 1);

static struct k_spinlock lock;
static struct hr_status state;
static int64_t pulse_at_ms;

void hr_want(enum hr_user user, bool on)
{
	if (on) {
		atomic_set_bit(&wanted, user);
	} else {
		atomic_clear_bit(&wanted, user);
	}
	k_sem_give(&wake);
}

bool hr_wanted(enum hr_user user)
{
	return atomic_test_bit(&wanted, user);
}

void hr_status(struct hr_status *status)
{
	K_SPINLOCK(&lock) {
		*status = state;
		if (k_uptime_get() - pulse_at_ms > FRESH_MS) {
			status->bpm = 0;
		}
	}
}

bool hr_read(uint8_t *bpm)
{
	struct hr_status status;

	hr_status(&status);
	*bpm = status.bpm;
	return status.on && status.worn && status.bpm != 0;
}

void hr_hw_event(void)
{
	k_sem_give(&wake);
}

/* The accelerometer, read at its own pace and taken with each lot of light. */

static struct k_spinlock accel_lock;
static int16_t accel[HR_HW_BATCH_MAX][3];
static unsigned int accel_n;

static void accel_read(struct k_work *work)
{
	int32_t mg[3];

	ARG_UNUSED(work);

	if (motion_hw_accel(mg) != 0) {
		return;
	}
	K_SPINLOCK(&accel_lock) {
		if (accel_n == HR_HW_BATCH_MAX) {
			/* The light is late: the oldest goes. */
			memmove(accel[0], accel[1], sizeof(accel) - sizeof(accel[0]));
			accel_n--;
		}
		for (int i = 0; i < 3; i++) {
			accel[accel_n][i] = (int16_t)CLAMP(mg[i], INT16_MIN, INT16_MAX);
		}
		accel_n++;
	}
}
static K_WORK_DEFINE(accel_work, accel_read);

static void accel_tick(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	(void)k_work_submit(&accel_work);
}
static K_TIMER_DEFINE(accel_timer, accel_tick, NULL);

/*
 * What the sensor gave and what was made of it, kept on the user volume when
 * asked for ("hr raw on") and of every recording (CONFIG_UNA_HR_RAW_RECORDED),
 * in the lines of src/hr_ref.c's files so that scripts/hr-replay reads both.
 */

static atomic_t raw_wanted;
static struct fs_file_t raw_file;
static bool raw_open;
static char raw_path[48];

static void raw_put(const char *text, size_t len)
{
	if (raw_open && fs_write(&raw_file, text, len) < 0) {
		LOG_ERR("the samples are not being written");
		(void)fs_close(&raw_file);
		raw_open = false;
	}
}

static void raw_set(bool on)
{
	static const char head[] = "0 # hr raw, accel mg 20 Hz\n";
	struct tm tm;

	if (on == raw_open) {
		return;
	}
	if (!on) {
		(void)fs_close(&raw_file);
		raw_open = false;
		return;
	}
	if (clock_local(&tm)) {
		snprintf(raw_path, sizeof(raw_path), RAW_DIR "/raw-%02d%02d%02d-%02d%02d%02d.txt",
			 tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
			 tm.tm_sec);
	} else {
		snprintf(raw_path, sizeof(raw_path), RAW_DIR "/raw-up%u.txt",
			 k_uptime_get_32() / 1000);
	}
	storage_mkdir(STORAGE_ROOT "/Log");
	storage_mkdir(RAW_DIR);
	fs_file_t_init(&raw_file);
	raw_open = fs_open(&raw_file, raw_path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC) == 0;
	if (!raw_open) {
		LOG_ERR("%s cannot be made", raw_path);
		atomic_set(&raw_wanted, 0);
		return;
	}
	raw_put(head, sizeof(head) - 1);
}

static void raw_batch(const struct hr_hw_batch *batch, const struct hr_alg_sample *samples,
		      const struct hr_alg_out *out)
{
	static char text[1024];
	static unsigned int written;
	uint32_t now = k_uptime_get_32();
	size_t len;

	if (!raw_open) {
		return;
	}
	len = snprintf(text, sizeof(text), "%u P %u %u %u", now, batch->worn ? 1 : 2,
		       HR_HW_CHANNELS, batch->count);
	for (unsigned int i = 0; i < batch->count; i++) {
		len += snprintf(&text[len], sizeof(text) - len, " %u %u", batch->light[i][0],
				batch->light[i][1]);
	}
	len += snprintf(&text[len], sizeof(text) - len, "\n%u C", now);
	for (unsigned int i = 0; i < batch->count; i++) {
		len += snprintf(&text[len], sizeof(text) - len, " %u %u", batch->led[i],
				batch->led[i]);
	}
	raw_put(text, len);
	len = snprintf(text, sizeof(text), "\n%u A %u", now, batch->count);
	for (unsigned int i = 0; i < batch->count; i++) {
		len += snprintf(&text[len], sizeof(text) - len, " %d %d %d",
				samples[i].accel_mg[0], samples[i].accel_mg[1],
				samples[i].accel_mg[2]);
	}
	/* As the library's answers are written: 48 is "a pulse", then tenths. */
	len += snprintf(&text[len], sizeof(text) - len, "\n%u H %u %u %u %u\n", now,
			out->bpm10 != 0 ? 48 : 0, out->bpm10, out->quality, out->raw_bpm10);
	raw_put(text, len);
	if (raw_open && ++written % 16 == 0) {
		(void)fs_sync(&raw_file);
	}
}

static bool set_power(bool on)
{
	int ret = hr_hw_power(on, atomic_get(&always) != 0);

	hr_alg_reset();
	K_SPINLOCK(&accel_lock) {
		accel_n = 0;
	}
	if (on && ret == 0) {
		k_timer_start(&accel_timer, K_MSEC(ACCEL_MS), K_MSEC(ACCEL_MS));
	} else {
		k_timer_stop(&accel_timer);
	}
	K_SPINLOCK(&lock) {
		state.on = on && ret == 0;
		state.worn = false;
		state.bpm = 0;
		state.quality = 0;
		state.batches = 0;
		if (on) {
			state.present = ret == 0;
			state.failures += ret != 0;
		}
	}
	if (on && ret != 0) {
		LOG_ERR("no sensor: %d", ret);
		(void)hr_hw_power(false, false);
		/* Whoever wanted it must ask again. */
		atomic_clear(&wanted);
	} else {
		LOG_INF("sensor %s (wanted by %lx)", on ? "on" : "off", (long)atomic_get(&wanted));
	}
	ui_refresh();
	return on && ret == 0;
}

static void take(const struct hr_hw_batch *batch)
{
	static struct hr_alg_sample samples[HR_HW_BATCH_MAX];
	static int16_t last_accel[3];
	struct hr_alg_out out;
	unsigned int have;
	uint8_t bpm;
	bool changed;

	K_SPINLOCK(&accel_lock) {
		/* The newest of them are the light's; too few, and one is repeated. */
		have = accel_n;
		for (unsigned int i = 0; i < batch->count; i++) {
			int j = (int)have - (int)batch->count + (int)i;

			if (have > 0) {
				memcpy(last_accel, accel[CLAMP(j, 0, (int)have - 1)],
				       sizeof(last_accel));
			}
			memcpy(samples[i].accel_mg, last_accel, sizeof(last_accel));
		}
		accel_n = 0;
	}
	for (unsigned int i = 0; i < batch->count; i++) {
		samples[i].light = batch->light[i][0] / 2 + batch->light[i][1] / 2;
		samples[i].led = batch->led[i];
	}
	if (batch->worn || atomic_get(&always) != 0) {
		hr_alg_feed(samples, batch->count, &out);
	} else {
		hr_alg_reset();
		out = (struct hr_alg_out){0};
	}
	raw_batch(batch, samples, &out);

	bpm = (uint8_t)MIN((out.bpm10 + 5) / 10, UINT8_MAX);
	K_SPINLOCK(&lock) {
		changed = bpm != state.bpm;
		state.batches++;
		state.quality = out.quality;
		state.moving = out.moving;
		if (bpm != 0) {
			state.bpm = bpm;
			pulse_at_ms = k_uptime_get();
		} else if (k_uptime_get() - pulse_at_ms > FRESH_MS) {
			state.bpm = 0;
		}
	}
	if (changed) {
		ui_refresh();
	}
#if defined(CONFIG_BT_HRS)
	if (bpm != 0 && hr_wanted(HR_FOR_PHONE)) {
		(void)bt_hrs_notify(bpm);
	}
#endif
}

#if defined(CONFIG_BT_HRS)
/*
 * The standard service, for any app that reads a pulse: the sensor is on
 * while one listens. The stack says "no more" too when the link is lost.
 */
static void on_listening(bool enabled)
{
	hr_want(HR_FOR_PHONE, enabled);
}

static struct bt_hrs_cb hrs_cb = {
	.ntf_changed = on_listening,
};
#endif

static void hr_thread(void *p1, void *p2, void *p3)
{
	static struct hr_hw_batch batch;
	bool on = false;

#if defined(CONFIG_BT_HRS)
	(void)bt_hrs_cb_register(&hrs_cb);
#endif
	(void)hr_hw_power(false, false);
	while (true) {
		bool want = atomic_get(&wanted) != 0;
		bool worn, changed;

		if (want != on) {
			on = set_power(want);
		}
		raw_set(on && (atomic_get(&raw_wanted) != 0 ||
			       (IS_ENABLED(CONFIG_UNA_HR_RAW_RECORDED) &&
				hr_wanted(HR_FOR_RECORD))));
		if (!on) {
			k_sem_take(&wake, K_FOREVER);
			continue;
		}
		worn = hr_hw_worn();
		K_SPINLOCK(&lock) {
			changed = worn != state.worn;
			state.worn = worn;
			if (!worn) {
				state.bpm = 0;
			}
		}
		if (changed) {
			LOG_INF("%s", worn ? "worn" : "not worn");
			ui_refresh();
		}
		while (hr_hw_batch(&batch) == 0) {
			take(&batch);
		}
		k_sem_take(&wake, K_SECONDS(2));
	}
}
K_THREAD_DEFINE(hr_tid, 4096, hr_thread, NULL, NULL, NULL, 9, 0, 700);

/* hr [on [always]|off|raw on|raw off] */
static int cmd_hr(const struct shell *sh, size_t argc, char **argv)
{
	struct hr_status s;
	char text[96];

	if (argc > 2 && strcmp(argv[1], "raw") == 0) {
		atomic_set(&raw_wanted, strcmp(argv[2], "on") == 0);
		k_sem_give(&wake);
		return 0;
	}
	if (argc > 1) {
		bool on = strcmp(argv[1], "on") == 0;

		if (on) {
			atomic_set(&always, argc > 2 && strcmp(argv[2], "always") == 0);
		}
		hr_want(HR_FOR_SHELL, on);
		return 0;
	}
	hr_status(&s);
	shell_print(sh, "%s, %s, %s, %u bpm, quality %u, %s, %u batches, %u failures",
		    s.present ? "present" : "not seen", s.on ? "on" : "off",
		    s.worn ? "worn" : "not worn", s.bpm, s.quality, s.moving ? "moving" : "still",
		    s.batches, s.failures);
	hr_hw_describe(text, sizeof(text));
	shell_print(sh, "%s", text);
	if (raw_open) {
		shell_print(sh, "written to %s", raw_path);
	}
	return 0;
}
SHELL_CMD_ARG_REGISTER(hr, NULL,
		       "The pulse, or the sensor on or off, or its samples kept: "
		       "hr [on [always]|off|raw on|raw off]",
		       cmd_hr, 1, 2);
