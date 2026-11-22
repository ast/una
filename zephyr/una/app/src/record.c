/*
 * The recorder: see record.h.
 *
 * One thread does everything that touches the file: it takes the UI's
 * commands and, while recording, writes a record every second from the
 * latest fix. A recording is in /Activity/recording.fit until it is
 * finished and then gets its name; one found there at start is a recording
 * that power was lost in, and is finished then.
 */

/* gmtime_r, for the simulator's host C library. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "alert.h"
#include "baro.h"
#include "clock.h"
#include "gnss.h"
#include "record.h"
#include "storage.h"
#include "ui.h"

LOG_MODULE_REGISTER(record, LOG_LEVEL_INF);

#define ACTIVITY_DIR   STORAGE_ROOT "/Activity"
#define RECORDING_PATH ACTIVITY_DIR "/recording.fit"

#define SYNC_EVERY_S 10
#define LAP_CM       100000

/* Below this speed a move counts only once it is this long: a still receiver wanders. */
#define MOVING_MM_S  400
#define MOVED_M      10
/* Faster than anything worn on a wrist: a jump, not a move. */
#define JUMP_M_S     60
/*
 * Climbing counts in steps: above the noise of the barometer, or of GNSS
 * altitude where there is no barometer.
 */
#define ASCENT_STEP_MM      3000
#define ASCENT_STEP_GNSS_MM 5000
/*
 * The barometer's height is off by the weather. What it is off by is taken
 * from the receiver: the mean of the difference, over this many fixes once
 * there are that many.
 */
#define SEA_FIXES 64

enum command {
	CMD_START,
	CMD_PAUSE,
	CMD_RESUME,
	CMD_FINISH,
	CMD_LABEL,
	CMD_DISCARD,
};

struct command_msg {
	uint8_t command;
	uint8_t sport;
};

K_MSGQ_DEFINE(commands, sizeof(struct command_msg), 8, 1);

/* What the UI sees. */
static struct record_state shown;
static struct k_spinlock shown_lock;

/* The recorder thread's own. */
static struct record_state now;
static int64_t next_tick_ms;
static int64_t utc_minus_uptime_s;
static bool have_point;
static int64_t point_latitude, point_longitude, point_at_ms;
static bool have_altitude, altitude_by_air;
static int32_t altitude_ref_mm;
static int32_t sea_offset_mm;
static int sea_fixes;
static uint32_t ascent_mm;
static uint32_t distance_cm;
static uint32_t laps;
static char last_path[96];

static void publish(void)
{
	struct gnss_status receiver;

	gnss_status(&receiver);
	now.gnss = receiver.present;
	K_SPINLOCK(&shown_lock) {
		shown = now;
	}
}

void record_get(struct record_state *state)
{
	K_SPINLOCK(&shown_lock) {
		*state = shown;
	}
}

bool record_active(void)
{
	return shown.phase != RECORD_IDLE;
}

static void send(enum command command, enum fit_sport sport)
{
	struct command_msg msg = {.command = command, .sport = sport};

	k_msgq_put(&commands, &msg, K_NO_WAIT);
}

void record_start(void)
{
	send(CMD_START, 0);
}

void record_pause(void)
{
	send(CMD_PAUSE, 0);
}

void record_resume(void)
{
	send(CMD_RESUME, 0);
}

void record_finish(void)
{
	send(CMD_FINISH, 0);
}

void record_label(enum fit_sport sport)
{
	send(CMD_LABEL, sport);
}

void record_discard(void)
{
	send(CMD_DISCARD, 0);
}

static uint32_t fit_now(void)
{
	return fit_time(utc_minus_uptime_s + k_uptime_get() / 1000);
}

/*
 * Metres between two points near each other. The watch's C library has no
 * mathematics, and this needs little: a cosine good to a millionth below 90
 * degrees, and a square root.
 */
static double cosine(double x)
{
	double x2 = x * x;

	return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90))));
}

static double root(double v)
{
	double r = v > 1 ? v : 1;

	if (v <= 0) {
		return 0;
	}
	for (int i = 0; i < 40; i++) {
		r = (r + v / r) / 2;
	}
	return r;
}

static double metres(int64_t lat1, int64_t lon1, int64_t lat2, int64_t lon2)
{
	const double rad = 3.14159265358979323846 / 180e9, earth = 6371000.0;
	double north = (lat2 - lat1) * rad * earth;
	double east = (lon2 - lon1) * rad * earth * cosine((lat1 + lat2) / 2 * rad);

	return root(north * north + east * east);
}

/* By the pace of it: a walk, a run or a ride. */
static enum fit_sport guess_sport(uint32_t distance_cm, uint32_t timer_s)
{
	uint32_t m_per_h = timer_s > 0 ? (uint64_t)distance_cm * 36 / timer_s : 0;

	return m_per_h < 7000 ? FIT_WALK : m_per_h < 16000 ? FIT_RUN : FIT_RIDE;
}

/* End the open file and give it the name of when it began. */
static void close_file(enum fit_sport sport, uint32_t ascent_m)
{
	const struct fit_summary *sum = fit_summary();
	time_t began = fit_unix_time(sum->start) + clock_utc_offset();
	struct tm tm;
	char dir[40];
	int ret;

	ret = fit_end(sport, ascent_m, clock_utc_offset());
	if (ret < 0) {
		LOG_ERR("recording not ended: %d", ret);
		return;
	}
	gmtime_r(&began, &tm);
	snprintf(dir, sizeof(dir), ACTIVITY_DIR "/%04d%02d", tm.tm_year + 1900, tm.tm_mon + 1);
	(void)fs_mkdir(dir);
	snprintf(last_path, sizeof(last_path), "%s/activity_%04d%02d%02dT%02d%02d%02d.fit", dir,
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	ret = fs_rename(RECORDING_PATH, last_path);
	if (ret < 0) {
		LOG_ERR("recording not renamed: %d", ret);
		last_path[0] = '\0';
		return;
	}
	LOG_INF("saved %s: %u s, %u m", last_path, sum->records, sum->distance_cm / 100);
}

/* A recording that was never finished: finish it. */
static void recover(void)
{
	struct fs_dirent entry;

	if (fs_stat(RECORDING_PATH, &entry) < 0) {
		return;
	}
	if (fit_resume(RECORDING_PATH) < 0 || fit_summary()->records == 0) {
		LOG_WRN("unfinished recording with nothing in it: removed");
		if (fit_is_open()) {
			(void)fit_end(FIT_WALK, FIT_NONE, 0);
		}
		(void)fs_unlink(RECORDING_PATH);
		return;
	}
	LOG_WRN("finishing a recording that was cut off");
	close_file(guess_sport(fit_summary()->distance_cm, fit_summary()->records), FIT_NONE);
}

static void start(void)
{
	time_t utc = clock_utc();
	int ret;

	utc_minus_uptime_s = utc - k_uptime_get() / 1000;
	now = (struct record_state){.phase = RECORD_RUNNING};
	have_point = false;
	have_altitude = false;
	sea_fixes = 0;
	ascent_mm = 0;
	distance_cm = 0;
	laps = 0;

	(void)fs_mkdir(ACTIVITY_DIR);
	ret = fit_begin(RECORDING_PATH, fit_time(utc));
	if (ret == 0) {
		ret = fit_put(FIT_EVENT, (uint32_t[]){fit_now(), 0, FIT_EVENT_START});
	}
	if (ret < 0) {
		LOG_ERR("recording without a file: %d", ret);
	}
	now.saved = ret == 0;
	gnss_want(GNSS_FOR_RECORD, true);
	baro_want(BARO_FOR_RECORD, true);
	next_tick_ms = k_uptime_get() + 1000;
}

static void finish(void)
{
	gnss_want(GNSS_FOR_RECORD, false);
	baro_want(BARO_FOR_RECORD, false);
	now.phase = RECORD_IDLE;
	now.fix = false;
	now.speed_mm_s = 0;
	now.sport = guess_sport(distance_cm, now.timer_s);
	if (fit_is_open()) {
		(void)fit_put(FIT_EVENT, (uint32_t[]){fit_now(), 0, FIT_EVENT_STOP});
		if (fit_summary()->records > 0) {
			close_file(now.sport, now.ascent_m);
		} else {
			/* Started and finished within the second. */
			(void)fit_end(now.sport, FIT_NONE, 0);
			(void)fs_unlink(RECORDING_PATH);
			now.saved = false;
		}
	}
}

/* Height gained: what it rose by, each time it has risen a step from where it turned. */
static void climb(int32_t altitude_mm, bool by_air)
{
	int32_t step = by_air ? ASCENT_STEP_MM : ASCENT_STEP_GNSS_MM;

	if (!have_altitude || by_air != altitude_by_air) {
		/* The two are not on one scale: start over from this one. */
		have_altitude = true;
		altitude_by_air = by_air;
		altitude_ref_mm = altitude_mm;
	} else if (altitude_mm >= altitude_ref_mm + step) {
		ascent_mm += altitude_mm - altitude_ref_mm;
		altitude_ref_mm = altitude_mm;
	} else if (altitude_mm <= altitude_ref_mm - step) {
		altitude_ref_mm = altitude_mm;
	}
}

/* A second of recording. */
static void tick(void)
{
	struct baro_reading air;
	bool have_air = baro_read(&air);
	uint32_t values[] = {fit_now(), FIT_NO_POSITION, FIT_NO_POSITION, FIT_NONE, FIT_NONE,
			     FIT_NONE};
	struct gnss_fix fix;
	bool fresh = gnss_fix(&fix);
	int64_t latitude = fix.latitude, longitude = fix.longitude, at_ms = fix.at_ms;
	int32_t altitude_mm = fix.altitude_mm;
	uint32_t speed_mm_s = fix.speed_mm_s;

	now.timer_s++;
	now.fix = fresh;
	now.speed_mm_s = fresh ? speed_mm_s : 0;
	if (fresh) {
		if (!have_point) {
			have_point = true;
			point_latitude = latitude;
			point_longitude = longitude;
			point_at_ms = at_ms;
		} else if (at_ms > point_at_ms) {
			double m = metres(point_latitude, point_longitude, latitude, longitude);
			bool jump = m * 1000 / (at_ms - point_at_ms) > JUMP_M_S;

			if (!jump && (speed_mm_s >= MOVING_MM_S || m >= MOVED_M)) {
				distance_cm += (uint32_t)(m * 100);
			}
			if (jump || speed_mm_s >= MOVING_MM_S || m >= MOVED_M) {
				point_latitude = latitude;
				point_longitude = longitude;
				point_at_ms = at_ms;
			}
		}
		if (have_air) {
			sea_fixes = MIN(sea_fixes + 1, SEA_FIXES);
			sea_offset_mm += (altitude_mm - air.altitude_mm - sea_offset_mm) / sea_fixes;
		} else {
			climb(altitude_mm, false);
		}
		values[1] = fit_semicircles(latitude);
		values[2] = fit_semicircles(longitude);
		values[4] = speed_mm_s;
		values[5] = fit_altitude(altitude_mm);
	}
	if (have_air) {
		climb(air.altitude_mm, true);
		/* Into the file once it is known how far off the weather puts it. */
		values[5] = sea_fixes > 0 ? fit_altitude(air.altitude_mm + sea_offset_mm)
					  : FIT_NONE;
	}
	values[3] = distance_cm;
	now.distance_m = distance_cm / 100;
	now.ascent_m = ascent_mm / 1000;

	if (fit_is_open()) {
		int ret = fit_put(FIT_RECORD, values);

		/* A kilometre: a lap, and a nudge. */
		if (ret == 0 && distance_cm / LAP_CM > laps) {
			laps = distance_cm / LAP_CM;
			ret = fit_lap(false);
			alert_blip();
		}
		if (ret == 0 && now.timer_s % SYNC_EVERY_S == 0) {
			ret = fit_sync();
		}
		if (ret < 0) {
			LOG_ERR("record not written: %d", ret);
		}
	}
	next_tick_ms += 1000;
}

#define DISCARDED_DIR ACTIVITY_DIR "/discarded"

static void discard(void)
{
	const char *name = strrchr(last_path, '/');
	char path[96];
	int ret;

	/* Nothing is shown as the last one any more. */
	now = (struct record_state){0};
	if (name == NULL) {
		return;
	}
	(void)fs_mkdir(DISCARDED_DIR);
	snprintf(path, sizeof(path), DISCARDED_DIR "%s", name);
	ret = fs_rename(last_path, path);
	if (ret < 0) {
		LOG_ERR("recording not moved: %d", ret);
		return;
	}
	LOG_INF("discarded: now %s", path);
	last_path[0] = '\0';
}

static void handle(const struct command_msg *msg)
{
	switch (msg->command) {
	case CMD_START:
		if (now.phase == RECORD_IDLE) {
			start();
		}
		break;
	case CMD_PAUSE:
		if (now.phase == RECORD_RUNNING) {
			now.phase = RECORD_PAUSED;
			now.speed_mm_s = 0;
			if (fit_is_open()) {
				(void)fit_put(FIT_EVENT,
					      (uint32_t[]){fit_now(), 0, FIT_EVENT_STOP});
				(void)fit_sync();
			}
		}
		break;
	case CMD_RESUME:
		if (now.phase == RECORD_PAUSED) {
			now.phase = RECORD_RUNNING;
			/* What was moved while paused is not part of it. */
			have_point = false;
			if (fit_is_open()) {
				(void)fit_put(FIT_EVENT,
					      (uint32_t[]){fit_now(), 0, FIT_EVENT_START});
			}
			next_tick_ms = k_uptime_get() + 1000;
		}
		break;
	case CMD_FINISH:
		if (now.phase != RECORD_IDLE) {
			finish();
		}
		break;
	case CMD_LABEL:
		if (now.phase == RECORD_IDLE && now.sport != msg->sport) {
			now.sport = msg->sport;
			if (now.saved && last_path[0] != '\0') {
				int ret = fit_set_sport(last_path, msg->sport);

				if (ret < 0) {
					LOG_ERR("label not written: %d", ret);
				}
			}
		}
		break;
	case CMD_DISCARD:
		if (now.phase == RECORD_IDLE) {
			discard();
		}
		break;
	}
}

static void record_thread(void *p1, void *p2, void *p3)
{
	recover();
	publish();

	while (true) {
		struct command_msg msg;
		k_timeout_t wait = now.phase == RECORD_RUNNING ? K_TIMEOUT_ABS_MS(next_tick_ms)
							       : K_FOREVER;

		if (k_msgq_get(&commands, &msg, wait) == 0) {
			handle(&msg);
		} else {
			tick();
		}
		publish();
		ui_refresh();
	}
}
/* After the UI thread (10): a write to the disk must not hold up a frame. */
K_THREAD_DEFINE(record_tid, 4096, record_thread, NULL, NULL, NULL, 11, 0, 1000);

static int cmd_record(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const phases[] = {"idle", "running", "paused"};
	struct record_state state;

	record_get(&state);
	shell_print(sh, "%s, %u s, %u m, %u mm/s, +%u m, fix %d, saved %d, sport %d, file %s",
		    phases[state.phase], state.timer_s, state.distance_m, state.speed_mm_s,
		    state.ascent_m, state.fix, state.saved, state.sport,
		    last_path[0] != '\0' ? last_path : "none");
	return 0;
}
SHELL_CMD_REGISTER(record, NULL, "The recorder's state", cmd_record);
