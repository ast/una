/*
 * PixArt's own driver and heart-rate algorithm on the watch, with everything
 * they do on the bus written down (hr-ref.conf; never in the watch as built
 * otherwise).
 *
 * The PAH8316 has no public datasheet. What is known of its registers is in
 * PixArt's library, which the stock firmware links as a binary and which
 * reaches the chip only through four functions given to it: write, read,
 * wait, and the time. Here those four are ours, and each call is a line in a
 * file in /USER:/Log/PPG/, between the library's own log messages and what
 * it made of the samples. The open driver is written from those files, and
 * its pulse is held against the library's.
 *
 * A line is the uptime in ms, a letter, and what the letter says:
 *
 *   W rr vv ..      written from register rr on
 *   R rr vv ..      read from register rr on ("R rr nn !e": nn bytes failed)
 *   D ms            the library waited
 *   I ms            the interrupt line rose, at that uptime
 *   L n mod fn: ..  the library's log message, level n
 *   P t c n v ..    a batch: touch, channels, samples a channel, the samples
 *   C v ..          the LED setting each sample of it was taken with
 *   A n x y z ..    the accelerometer over the batch, n readings of three
 *   H m bpm10 t g   the algorithm's answer: its message code, the pulse in
 *                   tenths, how far it trusts it, the signal's grade
 *   # ..            what was asked of the library just then
 *
 * The sequence around the library is the stock firmware's (PAH8316LS.cpp,
 * PixArtHrSession.cpp in the kernel): both supplies on, 10 ms, init, start
 * with profile 1 (green, 20 Hz) in batches of 20 by interrupt; off is the
 * LEDs' supply, then the sensor's.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

/* The algorithm's header asks whether it is built for a PC. */
#define FW 1
#include <pah831x_main.h>
#include <pah8series_api_c.h>
#include <pah_port.h>

#include "clock.h"
#include "motion.h"
#include "storage.h"

LOG_MODULE_REGISTER(hr_ref, LOG_LEVEL_INF);

#define USER DT_PATH(zephyr_user)

static const struct device *const bus = DEVICE_DT_GET(DT_NODELABEL(i2c2));
static const struct gpio_dt_spec sensor_en = GPIO_DT_SPEC_GET(USER, ppg_en_gpios);
static const struct gpio_dt_spec led_en = GPIO_DT_SPEC_GET(USER, ppg_led_en_gpios);
static const struct gpio_dt_spec irq = GPIO_DT_SPEC_GET(USER, ppg_int_gpios);

#define ADDRESS     0x40
#define ATTEMPTS    3
#define POWER_ON_MS 10
#define POWER_OFF_MS 20
#define WATERMARK   20
#define PERIOD_MS   50
#define TOUCH_OFF   2 /* the library's word for no skin; 0 is "not known" */

/*
 * The accelerometer as the algorithm is told it: 8 g to the 16 bits (mode 2),
 * at 50 Hz. The stock firmware says 8 g too. Ours are in the watch's axes,
 * not the chip's.
 */
#define GSENSOR_MODE 2
#define ACCEL_MS     20
#define ACCEL_MAX    200

#define TRACE_DIR STORAGE_ROOT "/Log/PPG"

enum event {
	EV_START,
	EV_STOP,
	EV_IRQ,
};

K_MSGQ_DEFINE(hr_events, sizeof(uint8_t), 8, 1);

static struct k_spinlock state_lock;
static struct {
	bool on;
	uint16_t id;
	uint8_t touch;
	uint16_t touch_value;
	int bpm10;
	int trust;
	int grade;
	uint8_t answer;
	uint32_t batches;
	uint32_t bus_errors;
	uint32_t open_size;
	char path[48];
} state;

static atomic_t wanted_mode = ATOMIC_INIT(PAH831X_INIT_AUTO_MODE);
static atomic_t irq_at;

/* The file. All of it is the thread's own: the library is called from nowhere else. */

static struct fs_file_t file;
static bool file_open;
static bool line_start = true;
static char trace[4096];
static size_t trace_len;
static unsigned int flushes;

static void trace_flush(bool sync)
{
	if (file_open && trace_len > 0) {
		if (fs_write(&file, trace, trace_len) < 0) {
			LOG_ERR("the trace is not being written");
			(void)fs_close(&file);
			file_open = false;
		} else if (sync || ++flushes % 16 == 0) {
			(void)fs_sync(&file);
		}
	}
	trace_len = 0;
}

static void trace_raw(const char *text, size_t len)
{
	while (len > 0) {
		size_t n = MIN(len, sizeof(trace) - trace_len);

		memcpy(&trace[trace_len], text, n);
		trace_len += n;
		text += n;
		len -= n;
		line_start = text[-1] == '\n';
		if (trace_len == sizeof(trace)) {
			trace_flush(false);
		}
	}
}

static void trace_vput(const char *fmt, va_list args)
{
	char text[256];
	int len = vsnprintf(text, sizeof(text), fmt, args);

	if (len > 0) {
		trace_raw(text, MIN((size_t)len, sizeof(text) - 1));
	}
}

static void trace_put(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	trace_vput(fmt, args);
	va_end(args);
}

/* The start of a line of ours: the library's messages may have left one open. */
static void trace_line(char letter)
{
	if (!line_start) {
		trace_raw("\n", 1);
	}
	trace_put("%u %c", k_uptime_get_32(), letter);
}

static void trace_end(void)
{
	trace_raw("\n", 1);
}

static void trace_mark(const char *fmt, ...)
{
	va_list args;

	trace_line('#');
	trace_raw(" ", 1);
	va_start(args, fmt);
	trace_vput(fmt, args);
	va_end(args);
	trace_end();
}

static void trace_open(void)
{
	char path[sizeof(state.path)];
	struct tm tm;

	if (clock_local(&tm)) {
		snprintf(path, sizeof(path), TRACE_DIR "/ref-%02d%02d%02d-%02d%02d%02d.txt",
			 tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
			 tm.tm_sec);
	} else {
		snprintf(path, sizeof(path), TRACE_DIR "/ref-up%u.txt", k_uptime_get_32() / 1000);
	}
	(void)fs_mkdir(STORAGE_ROOT "/Log");
	(void)fs_mkdir(TRACE_DIR);
	fs_file_t_init(&file);
	file_open = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC) == 0;
	if (!file_open) {
		LOG_ERR("no trace: %s cannot be made", path);
		path[0] = '\0';
	}
	trace_len = 0;
	line_start = true;
	K_SPINLOCK(&state_lock) {
		strcpy(state.path, path);
	}
}

static void trace_close(void)
{
	trace_flush(true);
	if (file_open) {
		(void)fs_close(&file);
		file_open = false;
	}
}

/* What the library asks of the firmware. */

/* The full C library it brings with it looks for this at exit, which never comes. */
void _fini(void)
{
}

static void bus_failed(void)
{
	K_SPINLOCK(&state_lock) {
		state.bus_errors++;
	}
}

static pah_ret plat_write(uint8_t reg, const uint8_t *data, size_t len, void *arg)
{
	int ret = -EIO;

	ARG_UNUSED(arg);

	for (int attempt = 0; attempt < ATTEMPTS && ret != 0; attempt++) {
		ret = i2c_burst_write(bus, ADDRESS, reg, data, len);
	}
	trace_line('W');
	trace_put(" %02x", reg);
	for (size_t i = 0; i < len; i++) {
		trace_put(" %02x", data[i]);
	}
	if (ret != 0) {
		trace_put(" !%d", ret);
		bus_failed();
	}
	trace_end();
	return ret == 0 ? PAH_RET_SUCCESS : PAH_RET_FAILED;
}

static pah_ret plat_read(uint8_t reg, uint8_t *data, size_t len, void *arg)
{
	int ret = -EIO;

	ARG_UNUSED(arg);

	for (int attempt = 0; attempt < ATTEMPTS && ret != 0; attempt++) {
		ret = i2c_burst_read(bus, ADDRESS, reg, data, len);
	}
	trace_line('R');
	trace_put(" %02x", reg);
	if (ret != 0) {
		trace_put(" %u !%d", (unsigned int)len, ret);
		bus_failed();
	} else {
		for (size_t i = 0; i < len; i++) {
			trace_put(" %02x", data[i]);
		}
	}
	trace_end();
	return ret == 0 ? PAH_RET_SUCCESS : PAH_RET_FAILED;
}

static void plat_delay(uint32_t ms, void *arg)
{
	ARG_UNUSED(arg);

	trace_line('D');
	trace_put(" %u", ms);
	trace_end();
	k_msleep(ms);
}

static uint32_t plat_ticks(void *arg)
{
	ARG_UNUSED(arg);

	return k_uptime_get_32();
}

void pah_port_log(uint8_t level, bool with_meta, const char *module, const char *func, int line,
		  const char *fmt, ...)
{
	va_list args;

	ARG_UNUSED(line);

	if (with_meta) {
		trace_line('L');
		trace_put(" %u %s %s: ", level, module != NULL ? module : "-",
			  func != NULL ? func : "-");
	}
	va_start(args, fmt);
	trace_vput(fmt, args);
	va_end(args);
}

/* The accelerometer, read at its own pace and handed over with each batch. */

static struct k_spinlock accel_lock;
static int16_t accel[ACCEL_MAX * 3];
static unsigned int accel_n;

static void accel_read(struct k_work *work)
{
	int32_t mg[3];

	ARG_UNUSED(work);

	if (motion_hw_accel(mg) != 0) {
		return;
	}
	K_SPINLOCK(&accel_lock) {
		if (accel_n < ACCEL_MAX) {
			for (int i = 0; i < 3; i++) {
				accel[accel_n * 3 + i] =
					CLAMP(mg[i] * 32768 / (2000 << GSENSOR_MODE), INT16_MIN,
					      INT16_MAX);
			}
			accel_n++;
		}
	}
}
static K_WORK_DEFINE(accel_work, accel_read);

static void accel_tick(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	(void)k_work_submit(&accel_work);
}
static K_TIMER_DEFINE(accel_timer, accel_tick, NULL);

static unsigned int accel_take(int16_t *out)
{
	unsigned int n;

	K_SPINLOCK(&accel_lock) {
		n = accel_n;
		memcpy(out, accel, n * 3 * sizeof(int16_t));
		accel_n = 0;
	}
	return n;
}

/* The algorithm, opened with the stock firmware's settings. */

static void *alg_memory;
static pah8series_data_t alg_data;

static void alg_set(pah8series_param_idx_t param, float value)
{
	(void)pah8series_set_param(param, value);
}

static bool alg_open(uint32_t channels)
{
	uint32_t size = pah8series_query_open_size();

	K_SPINLOCK(&state_lock) {
		state.open_size = size;
	}
	alg_memory = malloc(size);
	if (alg_memory == NULL) {
		trace_mark("no %u bytes for the algorithm", size);
		return false;
	}
	if (pah8series_open(alg_memory) != MSG_SUCCESS) {
		trace_mark("pah8series_open failed");
		free(alg_memory);
		alg_memory = NULL;
		return false;
	}
	trace_mark("algorithm %u open, %u bytes", pah8series_version(), size);
	alg_set(PAH8SERIES_PARAM_IDX_GSENSOR_MODE, GSENSOR_MODE);
	alg_set(PAH8SERIES_PARAM_IDX_PPG_CH_NUM, channels);
	alg_set(PAH8SERIES_PARAM_IDX_HAS_IR_CH, 0);
	alg_set(PAH8SERIES_PARAM_IDX_SET_FLAG_LIMIT_HR_UB, 1);
	alg_set(PAH8SERIES_PARAM_IDX_SET_LIMIT_HR_UB, 220);
	alg_set(PAH8SERIES_PARAM_IDX_SET_FLAG_LIMIT_HR_LB, 1);
	alg_set(PAH8SERIES_PARAM_IDX_SET_LIMIT_HR_LB, 110);
	alg_set(PAH8SERIES_PARAM_IDX_IS_8002, 0);
	alg_set(PAH8SERIES_PARAM_IDX_SET_FALG_INIT_HR_LBUB_BY_SIG_QUAL, 1);
	alg_set(PAH8SERIES_PARAM_IDX_SET_FLAG_PREDICTION_FEEDBACK, 1);
	alg_set(PAH8SERIES_PARAM_IDX_SET_FLAG_PREDICTION_SMOOTH, 1);
	alg_set(PAH8SERIES_PARAM_IDX_SET_SMOOTH_TRUST_LEVEL_TH, 1);
	alg_set(PAH8SERIES_PARAM_IDX_SET_FLAG_EN_CHANNEL_QUALITY, 1);
	alg_set(PAH8SERIES_PARAM_IDX_SET_FLAG_SUPPORT_FFT120, 1);
	alg_set(PAH8SERIES_PARAM_IDX_SET_IIR_LP_ALPHA_STATIC, 0.8f);
	alg_set(PAH8SERIES_PARAM_IDX_SET_IIR_LP_MOTION_TO_STATIC_COUNT, 40);
	alg_set(PAH8SERIES_PARAM_IDX_SET_MOTION_JMP_DIFF_IDX_THR1, 18);
	return true;
}

static void alg_close(void)
{
	if (alg_memory == NULL) {
		return;
	}
	(void)pah8series_close();
	free(alg_memory);
	alg_memory = NULL;
	alg_data.frame_count = 0;
	K_SPINLOCK(&state_lock) {
		state.bpm10 = 0;
		state.trust = 0;
		state.grade = 0;
	}
}

static void batch(void)
{
	static int32_t ppg[128];
	static int16_t mems[ACCEL_MAX * 3];
	ppg_data_out out = get_ppg_data_out();
	size_t n = out.ch_num * out.sample_num_per_ch;
	unsigned int mems_n = accel_take(mems);
	float hr = 0;
	int trust = 0;
	int16_t grade = 0;
	uint8_t answer;

	if (n == 0 || n > ARRAY_SIZE(ppg)) {
		trace_mark("a batch of %u samples is not taken", (unsigned int)n);
		return;
	}
	memcpy(ppg, out.ppg_raw_data, n * sizeof(ppg[0]));

	trace_line('P');
	trace_put(" %u %u %u", out.is_touched, out.ch_num, (unsigned int)out.sample_num_per_ch);
	for (size_t i = 0; i < n; i++) {
		trace_put(" %d", (int)ppg[i]);
	}
	trace_end();
	trace_line('C');
	for (size_t i = 0; i < n; i++) {
		trace_put(" %u", out.ppg_dac_data[i]);
	}
	trace_end();
	trace_line('A');
	trace_put(" %u", mems_n);
	for (unsigned int i = 0; i < mems_n * 3; i++) {
		trace_put(" %d", mems[i]);
	}
	trace_end();

	if (alg_memory == NULL && !alg_open(out.ch_num)) {
		return;
	}
	alg_data.frame_count++;
	alg_data.time = out.duration;
	alg_data.touch_flag = out.is_touched < TOUCH_OFF ? 1 : 0;
	alg_data.nf_ppg_channel = out.ch_num;
	alg_data.nf_ppg_per_channel = out.sample_num_per_ch;
	alg_data.ppg_data = ppg;
	alg_data.nf_mems = mems_n;
	alg_data.mems_data = mems;
	answer = pah8series_entrance(&alg_data);
	if (answer == MSG_HR_READY) {
		(void)pah8series_get_hr(&hr);
		(void)pah8series_get_hr_trust_level(&trust);
		(void)pah8series_get_signal_grade(&grade);
	}
	trace_line('H');
	trace_put(" %u %d %d %d", answer, (int)(hr * 10), trust, grade);
	trace_end();
	K_SPINLOCK(&state_lock) {
		state.batches++;
		state.answer = answer;
		if (answer == MSG_HR_READY) {
			state.bpm10 = (int)(hr * 10);
			state.trust = trust;
			state.grade = grade;
		}
	}
}

/* The sensor. */

static struct gpio_callback irq_callback;
static bool ppg_was_on;

static void on_irq(const struct device *port, struct gpio_callback *callback, uint32_t pins)
{
	uint8_t event = EV_IRQ;

	ARG_UNUSED(port);
	ARG_UNUSED(callback);
	ARG_UNUSED(pins);

	atomic_set(&irq_at, k_uptime_get_32());
	(void)k_msgq_put(&hr_events, &event, K_NO_WAIT);
}

static void power_off(void)
{
	(void)gpio_pin_interrupt_configure_dt(&irq, GPIO_INT_DISABLE);
	gpio_pin_set_dt(&led_en, 0);
	gpio_pin_set_dt(&sensor_en, 0);
	k_msleep(POWER_OFF_MS);
}

static void stop(void)
{
	bool on;

	K_SPINLOCK(&state_lock) {
		on = state.on;
		state.on = false;
		state.touch = 0;
	}
	if (!on) {
		return;
	}
	k_timer_stop(&accel_timer);
	(void)gpio_pin_interrupt_configure_dt(&irq, GPIO_INT_DISABLE);
	trace_mark("pah_sensor_stop");
	pah_sensor_stop();
	alg_close();
	trace_mark("supplies off");
	power_off();
	trace_close();
}

static void start(void)
{
	static pah_plat plat = {
		.type = PAH_PLAT_COMM_I2C,
		.max_length = 255,
		.write = plat_write,
		.read = plat_read,
		.delay_ms = plat_delay,
		.ticks = plat_ticks,
	};
	uint8_t mode = atomic_get(&wanted_mode);
	uint16_t id;

	stop();
	trace_open();
	trace_mark("supplies on, mode %u", mode);
	gpio_pin_set_dt(&sensor_en, 1);
	gpio_pin_set_dt(&led_en, 1);
	k_msleep(POWER_ON_MS);

	trace_mark("pah831x_sensor_init");
	id = pah831x_sensor_init(&plat);
	trace_mark("product %04x", id);
	K_SPINLOCK(&state_lock) {
		state.id = id;
		state.batches = 0;
		state.bus_errors = 0;
	}
	if (id == 0) {
		/* The library has kept nothing of ours and must not be called again. */
		LOG_ERR("the sensor did not answer as a PAH831x");
		power_off();
		trace_close();
		return;
	}
	/*
	 * Listened to before the start; the stock firmware does it after. The
	 * line rises within a few ms of the start, and the library, asked two
	 * seconds late about a rise nobody saw, set the chip up anew.
	 */
	(void)gpio_pin_interrupt_configure_dt(&irq, GPIO_INT_EDGE_TO_ACTIVE);
	trace_mark("pah831x_sensor_start, profile 1, batches of %u, %u ms", WATERMARK, PERIOD_MS);
	pah831x_sensor_start(PAH831X_WORK_DRI, mode, PAH831X_PPG_REG_SETTING_1,
			     PAH831X_ECG_REG_SETTING_IDLE, WATERMARK, (float)PERIOD_MS);
	trace_mark("started");
	trace_flush(true);

	ppg_was_on = false;
	K_SPINLOCK(&accel_lock) {
		accel_n = 0;
	}
	k_timer_start(&accel_timer, K_MSEC(ACCEL_MS), K_MSEC(ACCEL_MS));
	K_SPINLOCK(&state_lock) {
		state.on = true;
	}
}

static void service(bool by_edge)
{
	bool ppg_on;
	bool touch_changed;
	uint8_t touch;

	trace_line('I');
	if (by_edge) {
		trace_put(" %u", (uint32_t)atomic_get(&irq_at));
	} else {
		trace_put(" high with no edge seen");
	}
	trace_end();

	pah_interrupt_handle();
	pah831x_sensor_task();
	if (get_ppg_data_ready()) {
		batch();
		clear_ppg_data_ready();
	}
	ppg_on = get_ppg_enable() != 0;
	if (ppg_on != ppg_was_on) {
		ppg_was_on = ppg_on;
		trace_mark("PPG %s", ppg_on ? "on" : "off");
		if (!ppg_on) {
			alg_close();
		}
	}
	touch = get_pah831x_touch();
	K_SPINLOCK(&state_lock) {
		touch_changed = touch != state.touch;
		state.touch = touch;
	}
	if (touch_changed) {
		trace_mark("touch %u", touch);
	}
	trace_flush(false);
}

static void hr_ref_thread(void)
{
	bool on;
	uint8_t event;

	if (!device_is_ready(bus) || !gpio_is_ready_dt(&sensor_en) || !gpio_is_ready_dt(&led_en) ||
	    !gpio_is_ready_dt(&irq)) {
		LOG_ERR("no bus or pins");
		return;
	}
	gpio_pin_configure_dt(&led_en, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&sensor_en, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&irq, GPIO_INPUT);
	gpio_init_callback(&irq_callback, on_irq, BIT(irq.pin));
	gpio_add_callback(irq.port, &irq_callback);

	for (;;) {
		int ret = k_msgq_get(&hr_events, &event, K_SECONDS(2));

		K_SPINLOCK(&state_lock) {
			on = state.on;
		}
		if (ret != 0) {
			/* An edge that came before it was listened for is never seen. */
			if (on && gpio_pin_get_dt(&irq) > 0) {
				service(false);
			}
			continue;
		}
		switch (event) {
		case EV_START:
			start();
			break;
		case EV_STOP:
			stop();
			break;
		case EV_IRQ:
			if (on) {
				service(true);
			}
			break;
		}
	}
}
K_THREAD_DEFINE(hr_ref_tid, 8192, hr_ref_thread, NULL, NULL, NULL, 9, 0, 1000);

/* hrref [on [auto|always|touch]|off] */
static int cmd_hrref(const struct shell *sh, size_t argc, char **argv)
{
	__typeof__(state) s;
	uint8_t event;

	if (argc > 1) {
		if (strcmp(argv[1], "on") == 0) {
			uint8_t mode = PAH831X_INIT_AUTO_MODE;

			if (argc > 2 && strcmp(argv[2], "always") == 0) {
				mode = PAH831X_INIT_ALWAYS_RUN;
			} else if (argc > 2 && strcmp(argv[2], "touch") == 0) {
				mode = PAH831X_INIT_TOUCH_ONLY;
			}
			atomic_set(&wanted_mode, mode);
			event = EV_START;
		} else {
			event = EV_STOP;
		}
		return k_msgq_put(&hr_events, &event, K_MSEC(100));
	}
	K_SPINLOCK(&state_lock) {
		s = state;
	}
	shell_print(sh, "%s, product %04x, touch %u, %u batches, %u bus errors",
		    s.on ? "on" : "off", s.id, s.touch, s.batches, s.bus_errors);
	shell_print(sh, "%d.%d bpm, trust %d, grade %d, last answer 0x%02x, algorithm %u bytes",
		    s.bpm10 / 10, s.bpm10 % 10, s.trust, s.grade, s.answer, s.open_size);
	shell_print(sh, "trace %s", s.path[0] != '\0' ? s.path : "none");
	return 0;
}
SHELL_CMD_ARG_REGISTER(hrref, NULL,
		       "PixArt's library on the sensor, traced: hrref [on [auto|always|touch]|off]",
		       cmd_hrref, 1, 2);
