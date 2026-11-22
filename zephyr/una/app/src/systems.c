/*
 * The watch's systems, shown the way an aircraft shows its own: an overview
 * with every subsystem as a block on the power lines, and a page for each
 * with its figures. Colours as in draw.h; what is off or unknown is grey.
 */

#include <stdio.h>
#include <stdlib.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zephyr/app_version.h>
#ifdef CONFIG_MCUBOOT_IMG_MANAGER
#include <zephyr/dfu/mcuboot.h>
#endif

#include "alert.h"
#include "bluetooth.h"
#include "clock.h"
#include "draw.h"
#include "gnss.h"
#include "phone.h"
#include "power.h"
#include "storage.h"
#include "ui.h"

#define SMALL (&lv_font_montserrat_12)
#define TEXT  (&lv_font_montserrat_14)

/* Small text in the grey of the lines breaks up on the panel. */
#define INK(rgb) ((rgb) == DRAW_GREY ? DRAW_DIM : (rgb))

static lv_obj_t *canvas;
static struct power_status st;
static struct power_stats stats;
static struct gnss_status gnss;
static struct bluetooth_status ble;

/* PCA9420 charger states. */
static const char *charger_name(void)
{
	static const char *const names[] = {"IDLE", "QUAL", "LOW", "CC", "CV", "TOP", "DONE"};

	if (!st.usb_power) {
		return "OFF";
	}
	return st.charger_state < ARRAY_SIZE(names) ? names[st.charger_state] : "?";
}

static uint32_t battery_colour(void)
{
	return st.percent <= 10 ? DRAW_RED : st.percent <= 25 ? DRAW_AMBER : DRAW_GREEN;
}

/* Why the watch last started, read once: the flags add up until cleared. */
static const char *reset_cause = "---";
static bool reset_fault;

#ifdef CONFIG_HWINFO
static int reset_cause_init(void)
{
	uint32_t cause;

	if (hwinfo_get_reset_cause(&cause) < 0) {
		return 0;
	}
	hwinfo_clear_reset_cause();
	reset_fault = (cause & (RESET_WATCHDOG | RESET_BROWNOUT)) != 0;
	reset_cause = cause & RESET_WATCHDOG   ? "WATCHDOG"
		      : cause & RESET_BROWNOUT ? "BROWNOUT"
		      : cause & RESET_SOFTWARE ? "SOFTWARE"
		      : cause & RESET_POR      ? "POWER ON"
		      : cause & RESET_PIN      ? "PIN"
					       : "OTHER";
	return 0;
}
SYS_INIT(reset_cause_init, APPLICATION, 90);
#endif

/* A current in mA with two decimals. */
static const char *milliamps(int microamps)
{
	static char text[16];

	snprintf(text, sizeof(text), "%s%d.%02d", microamps < 0 ? "-" : "",
		 abs(microamps) / 1000, abs(microamps) % 1000 / 10);
	return text;
}

/* A line between two points with an arrowhead in the middle when it carries. */
static void flow(int x1, int y1, int x2, int y2, bool on)
{
	uint32_t rgb = on ? DRAW_GREEN : DRAW_GREY;
	int mx = (x1 + x2) / 2, my = (y1 + y2) / 2;
	int dx = x2 > x1 ? 1 : x2 < x1 ? -1 : 0;
	int dy = y2 > y1 ? 1 : y2 < y1 ? -1 : 0;

	draw_line(x1, y1, x2, y2, 1, rgb);
	if (on) {
		draw_line(mx + 3 * dx, my + 3 * dy, mx - 3 * dx - 4 * dy, my - 3 * dy - 4 * dx, 1,
			  rgb);
		draw_line(mx + 3 * dx, my + 3 * dy, mx - 3 * dx + 4 * dy, my - 3 * dy + 4 * dx, 1,
			  rgb);
	}
}

/* A subsystem: its name and its one figure, in the colour of its state. */
#define BLOCK_W 46
#define BLOCK_H 30

static void block(int x, int y, const char *name, const char *value, uint32_t rgb)
{
	draw_box(x, y, BLOCK_W, BLOCK_H, rgb, false);
	draw_text(x, y - 7, name, SMALL, rgb == DRAW_GREY ? DRAW_DIM : DRAW_WHITE, DRAW_CENTRE);
	draw_text(x, y + 6, value, TEXT, INK(rgb), DRAW_CENTRE);
}

/* The overview */

static void paint_overview(void)
{
	const int y1 = -52, bus = -25, y2 = 2, sub = 28, y3 = 52;
	static const int x3[] = {-69, -23, 23, 69};
	bool charging = st.usb_power && st.microamps > 0;
	bool sound = alert_sound() || alert_vibration();
	struct storage_info disk;
	char text[16];

	draw_text(0, -92, "SYSTEMS", TEXT, DRAW_DIM, DRAW_CENTRE);

	/* Supply: USB through the charger, which also joins the battery. */
	flow(-60 + BLOCK_W / 2, y1, -BLOCK_W / 2, y1, st.usb_power);
	if (charging) {
		flow(BLOCK_W / 2, y1, 60 - BLOCK_W / 2, y1, true);
	} else {
		flow(60 - BLOCK_W / 2, y1, BLOCK_W / 2, y1, st.microamps < 0);
	}
	block(-60, y1, "USB", st.usb_power ? "ON" : "---", st.usb_power ? DRAW_GREEN : DRAW_GREY);
	block(0, y1, "CHG", charger_name(), st.usb_power ? DRAW_GREEN : DRAW_GREY);
	snprintf(text, sizeof(text), "%d%%", st.percent);
	block(60, y1, "BAT", text, battery_colour());

	/* The bus and what hangs on it. */
	draw_line(0, y1 + BLOCK_H / 2, 0, bus, 1, DRAW_GREEN);
	draw_line(-60, bus, 60, bus, 1, DRAW_GREEN);
	for (int x = -60; x <= 60; x += 60) {
		draw_line(x, bus, x, y2 - BLOCK_H / 2, 1, DRAW_GREEN);
	}
	draw_line(-31, bus, -31, sub, 1, DRAW_GREEN);
	draw_line(31, bus, 31, sub, 1, DRAW_GREEN);
	draw_line(x3[0], sub, x3[3], sub, 1, DRAW_GREEN);

	snprintf(text, sizeof(text), "%d.%d%%", stats.awake_permille / 10,
		 stats.awake_permille % 10);
	block(-60, y2, "MCU", text, DRAW_GREEN);
	block(0, y2, "DISP", "ON", DRAW_GREEN);
	if (storage_info(&disk, false)) {
		block(60, y2, "STOR", "OK", DRAW_GREEN);
	} else {
		block(60, y2, "STOR", "---", DRAW_GREY);
	}

	/* The receiver: how many satellites its fix is of, or that it is looking. */
	if (!gnss.on) {
		block(x3[0], y3, "GNSS", gnss.present ? "OFF" : "---", DRAW_GREY);
	} else if (gnss.fix) {
		snprintf(text, sizeof(text), "3D %d", gnss.used);
		block(x3[0], y3, "GNSS", text, DRAW_GREEN);
	} else {
		block(x3[0], y3, "GNSS", "ACQ", DRAW_AMBER);
	}
	/* The radio: with a phone, looking for one, or held in reset. */
	if (ble.state == BLUETOOTH_CONNECTED) {
		block(x3[1], y3, "BLE", ble.listening ? "LINK" : "CONN", DRAW_GREEN);
	} else if (ble.state == BLUETOOTH_FAILED) {
		block(x3[1], y3, "BLE", "FAIL", DRAW_AMBER);
	} else if (ble.state == BLUETOOTH_OFF) {
		block(x3[1], y3, "BLE", "RST", DRAW_GREY);
	} else {
		block(x3[1], y3, "BLE", "ADV", DRAW_GREEN);
	}
	/* Not built yet: their supply is off. */
	block(x3[2], y3, "SENS", "OFF", DRAW_GREY);
	block(x3[3], y3, "ALRT", sound ? "ON" : "MUTE", sound ? DRAW_GREEN : DRAW_GREY);
	for (int i = 0; i < ARRAY_SIZE(x3); i++) {
		draw_line(x3[i], sub, x3[i], y3 - BLOCK_H / 2, 1,
			  (i == 0 && gnss.on) || (i == 1 && ble.state != BLUETOOTH_OFF) ||
					  (i == 3 && sound)
				  ? DRAW_GREEN
				  : DRAW_GREY);
	}
}

static void systems_enter(void)
{
	canvas = draw_create(paint_overview);
}

static uint32_t systems_update(void)
{
	power_last(&st);
	power_stats(&stats);
	gnss_status(&gnss);
	bluetooth_status(&ble);
	lv_obj_invalidate(canvas);
	/* A receiver that is on has news every second. */
	return gnss.on ? 1000 : 5000;
}

/* The pages */

enum page {
	PAGE_ELEC,
	PAGE_MCU,
	PAGE_STOR,
	PAGE_GNSS,
	PAGE_BLE,
	PAGE_ALRT,
	PAGE_COUNT,
};

static enum page page;

static void title(const char *name)
{
	char text[24];

	snprintf(text, sizeof(text), "%s  %d/%d", name, page + 1, PAGE_COUNT);
	draw_text(0, -92, text, TEXT, DRAW_DIM, DRAW_CENTRE);
}

/* A name and a value with its unit, the value starting at x. */
static void figure(int x, int y, const char *name, const char *value, const char *unit,
		   uint32_t rgb)
{
	draw_text(x - 6, y + 1, name, SMALL, DRAW_DIM, DRAW_RIGHT);
	x += draw_text(x, y, value, TEXT, INK(rgb), DRAW_LEFT);
	draw_text(x + 3, y + 1, unit, SMALL, DRAW_WHITE, DRAW_LEFT);
}

/* The battery current of the last hour, a minute a point, scaled to fit. */
static void trend(int y, int w, int h)
{
	int32_t ua[POWER_TREND_N];
	int n = power_trend(ua);
	int32_t low = 0, high = 0;

	draw_box(0, y, w + 4, h + 4, DRAW_GREY, false);
	for (int i = 0; i < n; i++) {
		low = MIN(low, ua[i]);
		high = MAX(high, ua[i]);
	}
	if (n < 2 || high == low) {
		return;
	}
#define TREND_X(i) (-w / 2 + (i) * w / (POWER_TREND_N - 1))
#define TREND_Y(v) (y + h / 2 - (int)((int64_t)((v) - low) * h / (high - low)))
	/* Zero: above it the battery is charged, below it drained. */
	draw_line(-w / 2, TREND_Y(0), w / 2, TREND_Y(0), 1, DRAW_GREY);
	for (int i = 1; i < n; i++) {
		draw_line(TREND_X(POWER_TREND_N - n + i - 1), TREND_Y(ua[i - 1]),
			  TREND_X(POWER_TREND_N - n + i), TREND_Y(ua[i]), 1, DRAW_GREEN);
	}
}

static void paint_elec(void)
{
	const int y = -56, w = 44, h = 24;
	bool charging = st.usb_power && st.microamps > 0;
	char text[16];

	title("ELEC");

	/* USB feeds the charger; the charger feeds the load and the battery. */
	flow(-64 + w / 2, y, -w / 2, y, st.usb_power);
	if (charging) {
		flow(w / 2, y, 64 - w / 2, y, true);
	} else {
		flow(64 - w / 2, y, w / 2, y, st.microamps < 0);
	}
	flow(0, y + h / 2, 0, y + 32, true);
	draw_box(-64, y, w, h, st.usb_power ? DRAW_GREEN : DRAW_GREY, false);
	draw_text(-64, y, "USB", SMALL, st.usb_power ? DRAW_WHITE : DRAW_DIM, DRAW_CENTRE);
	draw_box(0, y, w, h, st.usb_power ? DRAW_GREEN : DRAW_GREY, false);
	draw_text(0, y, "CHG", SMALL, st.usb_power ? DRAW_WHITE : DRAW_DIM, DRAW_CENTRE);
	draw_text(0, y - 20, charger_name(), SMALL, st.usb_power ? DRAW_GREEN : DRAW_DIM,
		  DRAW_CENTRE);
	draw_box(64, y, w, h, battery_colour(), false);
	draw_text(64, y, "BAT", SMALL, DRAW_WHITE, DRAW_CENTRE);
	draw_text(0, y + 40, "LOAD", SMALL, DRAW_WHITE, DRAW_CENTRE);

	/* Under the boxes: what the charger may draw, and the cell's voltage. */
	if (st.usb_power) {
		snprintf(text, sizeof(text), "%u mA", st.charge_limit_ma);
		draw_text(-64, y + 22, text, SMALL, DRAW_CYAN, DRAW_CENTRE);
	}
	snprintf(text, sizeof(text), "%d mV", st.millivolts);
	draw_text(64, y + 22, text, SMALL, DRAW_GREEN, DRAW_CENTRE);

	figure(-70, 4, "I", milliamps(st.microamps), "mA", DRAW_GREEN);
	snprintf(text, sizeof(text), "%d", st.percent);
	figure(38, 4, "SOC", text, "%", battery_colour());
	figure(-70, 24, "1H", milliamps(stats.mean_ua), "mA", DRAW_GREEN);
	snprintf(text, sizeof(text), "%d", st.remaining_mah);
	figure(38, 24, "CAP", text, "mAh", DRAW_GREEN);
	/* How long the battery lasts at the last hour's mean. */
	if (stats.mean_ua < -10) {
		int hours = st.remaining_mah * 1000 / -stats.mean_ua;

		snprintf(text, sizeof(text), "%d", hours);
		figure(-70, 44, "END", text, "h", hours < 12 ? DRAW_AMBER : DRAW_GREEN);
	} else {
		figure(-70, 44, "END", "---", "", DRAW_GREY);
	}
	trend(78, 120, 28);
}

/* A line of a page that is a list of figures. */
static void line(int index, const char *name, const char *value, const char *unit, uint32_t rgb)
{
	figure(-14, -62 + index * 21, name, value, unit, rgb);
}

static void paint_mcu(void)
{
	int64_t up_min = k_uptime_get() / 60000;
	char text[16];

	title("MCU");
	line(0, "VER", APP_VERSION_STRING, "", DRAW_WHITE);
#ifdef CONFIG_MCUBOOT_IMG_MANAGER
	/* An image on trial is reverted at the next reset. */
	if (boot_is_img_confirmed()) {
		line(1, "IMAGE", "CONFIRMED", "", DRAW_GREEN);
	} else {
		line(1, "IMAGE", "ON TRIAL", "", DRAW_AMBER);
	}
#else
	line(1, "IMAGE", "---", "", DRAW_GREY);
#endif
	if (up_min >= 24 * 60) {
		snprintf(text, sizeof(text), "%dd %02dh", (int)(up_min / 1440),
			 (int)(up_min / 60 % 24));
	} else {
		snprintf(text, sizeof(text), "%dh %02dm", (int)(up_min / 60), (int)(up_min % 60));
	}
	line(2, "UP", text, "", DRAW_GREEN);
	line(3, "RESET", reset_cause, "", reset_fault ? DRAW_AMBER : DRAW_WHITE);
	snprintf(text, sizeof(text), "%d.%d", stats.awake_permille / 10, stats.awake_permille % 10);
	line(4, "AWAKE", text, "%", DRAW_GREEN);
	snprintf(text, sizeof(text), "%u", stats.stops_per_min);
	line(5, "STOP", text, "/min", DRAW_GREEN);
	/* Held, the SoC cannot sleep: as it should be on USB only. */
	if (stats.stop_locked) {
		line(6, "LOCK", "HELD", "", st.usb_power ? DRAW_WHITE : DRAW_AMBER);
	} else {
		line(6, "LOCK", "FREE", "", DRAW_GREEN);
	}
}

static void paint_stor(void)
{
	struct storage_info disk;
	char text[16];

	title("STOR");
	if (!storage_info(&disk, true)) {
		line(1, "USER", "---", "", DRAW_GREY);
		line(2, "SIZE", "---", "", DRAW_GREY);
		line(3, "FREE", "---", "", DRAW_GREY);
		return;
	}
	line(1, "USER", "MOUNTED", "", DRAW_GREEN);
	snprintf(text, sizeof(text), "%u", disk.total_mb);
	line(2, "SIZE", text, "MB", DRAW_GREEN);
	snprintf(text, sizeof(text), "%u", disk.free_mb);
	line(3, "FREE", text, "MB", disk.free_mb < 100 ? DRAW_AMBER : DRAW_GREEN);
	/* How full it is. */
	if (disk.total_mb > 0) {
		const int w = 120;
		int used = w - (int)((uint64_t)disk.free_mb * w / disk.total_mb);

		draw_box(0, 36, w + 4, 12, DRAW_GREY, false);
		draw_box(-w / 2 + used / 2, 36, used, 8, DRAW_GREEN, true);
	}
}

/* The receiver: what it has, and how strongly it hears its best satellites. */
static void paint_gnss(void)
{
	const int base = 78, bar_w = 9, pitch = 12, full = 44; /* 44 px is 50 dB-Hz */
	char text[16];

	title("GNSS");
	if (!gnss.present) {
		line(1, "POWER", "---", "", DRAW_GREY);
		return;
	}
	line(0, "POWER", gnss.on ? "ON" : "OFF", "", gnss.on ? DRAW_GREEN : DRAW_GREY);
	if (gnss.on) {
		snprintf(text, sizeof(text), "%u", gnss.on_s);
		figure(52, -62, "", text, "s", DRAW_GREEN);
		line(1, "FIX", gnss.fix ? "3D" : "NONE", "", gnss.fix ? DRAW_GREEN : DRAW_AMBER);
		snprintf(text, sizeof(text), "%d / %d", gnss.used, gnss.seen);
		line(2, "SATS", text, "", gnss.seen > 0 ? DRAW_GREEN : DRAW_AMBER);
		if (gnss.first_fix_s >= 0) {
			snprintf(text, sizeof(text), "%d", gnss.first_fix_s);
			line(3, "TTFF", text, "s", DRAW_GREEN);
		} else {
			/* Nothing comes in at all: the receiver is not talking. */
			snprintf(text, sizeof(text), "%u", gnss.sentences);
			line(3, "NMEA", text, "", gnss.sentences > 0 ? DRAW_GREEN : DRAW_RED);
		}
		draw_line(-72, base, 72, base, 1, DRAW_GREY);
		for (int i = 0; i < GNSS_SNR_N; i++) {
			int h = MIN(gnss.snr[i], 50) * full / 50;
			int x = -66 + i * pitch;

			if (h > 0) {
				/* Under 25 dB-Hz a satellite is heard but of little use. */
				draw_box(x, base - h / 2, bar_w, h,
					 gnss.snr[i] >= 25 ? DRAW_GREEN : DRAW_DIM, true);
			}
		}
	}
	draw_text(0, 92, gnss_wanted(GNSS_FOR_PAGE) ? "TOP RIGHT: OFF" : "TOP RIGHT: ON", SMALL,
		  DRAW_DIM, DRAW_CENTRE);
}

static void paint_ble(void)
{
	static const char *const states[] = {"RESET", "START", "ADVERT", "CONN", "FAIL"};
	static const uint32_t colours[] = {DRAW_GREY, DRAW_WHITE, DRAW_GREEN, DRAW_GREEN,
					   DRAW_AMBER};
	struct phone_status phone;
	char text[20];

	phone_status(&phone);
	title("BLE");
	line(0, "RADIO", states[ble.state], "", colours[ble.state]);
	if (ble.state != BLUETOOTH_CONNECTED) {
		line(1, "PHONE", "---", "", DRAW_GREY);
	} else {
		/* Connected is not yet talking: Gadgetbridge has to open the line. */
		line(1, "PHONE", ble.listening ? "LINK" : "NO LINK", "",
		     ble.listening ? DRAW_GREEN : DRAW_AMBER);
		if (ble.listening && ble.rssi != 0) {
			snprintf(text, sizeof(text), "%d", ble.rssi);
			figure(40, -62 + 1 * 21, "", text, "dBm", DRAW_GREEN);
		}
		snprintf(text, sizeof(text), "%u", ble.interval_ms);
		line(2, "PACE", text, "ms", DRAW_GREEN);
		snprintf(text, sizeof(text), "%u", ble.latency);
		figure(52, -62 + 2 * 21, "", text, "skip", DRAW_GREEN);
		if (ble.connected_s >= 3600) {
			snprintf(text, sizeof(text), "%uh %02um", ble.connected_s / 3600,
				 ble.connected_s / 60 % 60);
		} else {
			snprintf(text, sizeof(text), "%um %02us", ble.connected_s / 60,
				 ble.connected_s % 60);
		}
		line(3, "FOR", text, "", DRAW_GREEN);
	}
	snprintf(text, sizeof(text), "%u / %u", ble.rx_bytes, ble.tx_bytes);
	line(4, "RX/TX", text, "B", DRAW_WHITE);
	snprintf(text, sizeof(text), "%u", phone.lines);
	line(5, "LINES", text, "", DRAW_WHITE);
	if (phone.time_set_at != 0) {
		time_t ago = clock_utc() - phone.time_set_at;

		snprintf(text, sizeof(text), "%d", (int)(ago / 60));
		line(6, "TIME", text, "min ago", DRAW_GREEN);
	} else {
		line(6, "TIME", "---", "", DRAW_GREY);
	}
	draw_text(0, 92, ble.own, SMALL, DRAW_DIM, DRAW_CENTRE);
}

static void paint_alrt(void)
{
	title("ALRT");
	line(1, "SOUND", alert_sound() ? "ON" : "OFF", "", alert_sound() ? DRAW_GREEN : DRAW_GREY);
	line(2, "VIB", alert_vibration() ? "ON" : "OFF", "",
	     alert_vibration() ? DRAW_GREEN : DRAW_GREY);
	draw_text(0, 30, "TOP RIGHT: TEST", SMALL, DRAW_DIM, DRAW_CENTRE);
}

static void paint_page(void)
{
	static void (*const paint[PAGE_COUNT])(void) = {paint_elec, paint_mcu, paint_stor,
							paint_gnss, paint_ble,  paint_alrt};

	paint[page]();
}

static void page_enter(void)
{
	canvas = draw_create(paint_page);
}

/* UP and DOWN go through the pages. */
static bool page_key(enum ui_key key, bool held)
{
	if (held) {
		return key != UI_KEY_BACK;
	}
	if (key == UI_KEY_UP) {
		page = (page + PAGE_COUNT - 1) % PAGE_COUNT;
	} else if (key == UI_KEY_DOWN) {
		page = (page + 1) % PAGE_COUNT;
	} else if (key == UI_KEY_SELECT && page == PAGE_ALRT) {
		alert_blip();
	} else if (key == UI_KEY_SELECT && page == PAGE_GNSS) {
		/* To look at the receiver without recording; see page_leave. */
		gnss_want(GNSS_FOR_PAGE, !gnss_wanted(GNSS_FOR_PAGE));
	} else {
		return key == UI_KEY_SELECT;
	}
	return true;
}

/* The receiver is not left on by having looked at it. */
static void page_leave(void)
{
	gnss_want(GNSS_FOR_PAGE, false);
}

static const struct ui_screen page_screen = {
	.name = "system page",
	.enter = page_enter,
	.update = systems_update,
	.key = page_key,
	.leave = page_leave,
};

static bool systems_key(enum ui_key key, bool held)
{
	ARG_UNUSED(held);

	if (key != UI_KEY_SELECT) {
		return false;
	}
	ui_push(&page_screen);
	return true;
}

const struct ui_screen systems_screen = {
	.name = "systems",
	.enter = systems_enter,
	.update = systems_update,
	.key = systems_key,
};
