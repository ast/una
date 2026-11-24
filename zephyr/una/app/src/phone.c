/*
 * The phone's side of the serial line, as Gadgetbridge speaks it to a
 * Bangle.js. It sends lines, each a piece of JavaScript for that watch to
 * run; two kinds matter here:
 *
 *   setTime(1795348800);E.setTimeZone(1.0);...
 *   GB({"t":"notify","id":7,"src":"Signal","title":"Ada","body":"On my way"})
 *
 * The object is a JavaScript literal, not JSON: text is Latin-1 with \xNN,
 * \uNNNN and octal escapes, and a string may come as atob("<base64>").
 * What the watch sends is JSON proper, one object to a line.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/app_version.h>

#include "alert.h"
#include "bluetooth.h"
#include "clock.h"
#include "phone.h"
#include "ui.h"

LOG_MODULE_REGISTER(phone, LOG_LEVEL_INF);

/* A notification's 400 characters of body, every one of them escaped. */
#define LINE_MAX 3072

static char line[LINE_MAX];
static size_t line_len;
static bool line_too_long;

static K_MUTEX_DEFINE(lock);
static struct phone_note notes[PHONE_NOTES];
static int note_count;
static uint32_t notes_version;
static struct phone_status status;

/* Reading the object */

static int hex(char c)
{
	return c >= '0' && c <= '9'   ? c - '0'
	       : c >= 'a' && c <= 'f' ? c - 'a' + 10
	       : c >= 'A' && c <= 'F' ? c - 'A' + 10
				      : -1;
}

/* The end of the string that starts at p, a quote. */
static const char *string_end(const char *p)
{
	for (p++; *p != '\0' && *p != '"'; p++) {
		if (*p == '\\' && p[1] != '\0') {
			p++;
		}
	}
	return *p == '"' ? p + 1 : p;
}

/* What follows the value that starts at p. */
static const char *value_end(const char *p)
{
	int depth = 0;

	while (*p != '\0') {
		if (*p == '"') {
			p = string_end(p);
			continue;
		}
		if (*p == '{' || *p == '[' || *p == '(') {
			depth++;
		} else if (*p == '}' || *p == ']' || *p == ')') {
			if (depth == 0) {
				break;
			}
			depth--;
		} else if (*p == ',' && depth == 0) {
			break;
		}
		p++;
	}
	return p;
}

/* The value of a key of the object that starts at obj, a brace. */
static const char *value_of(const char *obj, const char *key)
{
	size_t key_len = strlen(key);
	const char *p = obj + 1;

	while (*p == '"') {
		const char *name = p + 1;
		const char *end = string_end(p);

		if (*end != ':') {
			break;
		}
		if ((size_t)(end - 1 - name) == key_len && memcmp(name, key, key_len) == 0) {
			return end + 1;
		}
		p = value_end(end + 1);
		if (*p != ',') {
			break;
		}
		p++;
	}
	return NULL;
}

static bool int_of(const char *obj, const char *key, int32_t *value)
{
	const char *p = value_of(obj, key);

	if (p == NULL) {
		return false;
	}
	if (strncmp(p, "true", 4) == 0 || strncmp(p, "false", 5) == 0) {
		*value = *p == 't';
		return true;
	}
	if (*p != '-' && (*p < '0' || *p > '9')) {
		return false;
	}
	*value = strtol(p, NULL, 10);
	return true;
}

struct text {
	char *at;
	char *end; /* where the terminator goes at the latest */
	bool full;
};

/* One character more, as UTF-8; what does not fit whole is left out. */
static void put(struct text *out, uint32_t c)
{
	uint8_t utf8[4];
	size_t n;

	if (c == '\t' || c == '\r') {
		c = ' ';
	}
	if (c < 0x20 && c != '\n') {
		return;
	}
	if (c < 0x80) {
		utf8[0] = c;
		n = 1;
	} else if (c < 0x800) {
		utf8[0] = 0xc0 | (c >> 6);
		utf8[1] = 0x80 | (c & 0x3f);
		n = 2;
	} else if (c < 0x10000) {
		utf8[0] = 0xe0 | (c >> 12);
		utf8[1] = 0x80 | ((c >> 6) & 0x3f);
		utf8[2] = 0x80 | (c & 0x3f);
		n = 3;
	} else {
		utf8[0] = 0xf0 | (c >> 18);
		utf8[1] = 0x80 | ((c >> 12) & 0x3f);
		utf8[2] = 0x80 | ((c >> 6) & 0x3f);
		utf8[3] = 0x80 | (c & 0x3f);
		n = 4;
	}
	if (out->full || out->at + n > out->end) {
		out->full = true; /* and nothing after it either */
		return;
	}
	memcpy(out->at, utf8, n);
	out->at += n;
}

/* p is past the opening quote. */
static void put_string(struct text *out, const char *p)
{
	uint32_t high = 0;

	while (*p != '\0' && *p != '"') {
		uint32_t c = (uint8_t)*p++;

		if (c == '\\') {
			c = (uint8_t)*p++;
			if (c == 'n') {
				c = '\n';
			} else if (c == 't' || c == 'v' || c == 'f' || c == 'b') {
				c = ' ';
			} else if (c == 'x' && hex(p[0]) >= 0 && hex(p[1]) >= 0) {
				c = hex(p[0]) << 4 | hex(p[1]);
				p += 2;
			} else if (c == 'u' && hex(p[0]) >= 0 && hex(p[1]) >= 0 &&
				   hex(p[2]) >= 0 && hex(p[3]) >= 0) {
				c = hex(p[0]) << 12 | hex(p[1]) << 8 | hex(p[2]) << 4 | hex(p[3]);
				p += 4;
			} else if (c >= '0' && c <= '7') {
				c -= '0';
				for (int i = 0; i < 2 && *p >= '0' && *p <= '7'; i++) {
					c = c << 3 | (*p++ - '0');
				}
			} else if (c == '\0') {
				break;
			}
		}
		if (c == 0) {
			/* What follows is a picture of the text, for the Bangle.js. */
			break;
		}
		/* A character beyond the first 65536 comes as two halves. */
		if (c >= 0xd800 && c < 0xdc00) {
			high = c;
			continue;
		}
		if (c >= 0xdc00 && c < 0xe000) {
			c = high != 0 ? 0x10000 + ((high - 0xd800) << 10) + (c - 0xdc00) : '?';
		}
		high = 0;
		put(out, c);
	}
}

/* p is past the opening quote of base64, whose bytes are Latin-1. */
static void put_base64(struct text *out, const char *p)
{
	static const char digits[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	uint32_t bits = 0;
	int count = 0;

	for (; *p != '\0' && *p != '"' && *p != '='; p++) {
		const char *digit = strchr(digits, *p);

		if (digit == NULL) {
			continue;
		}
		bits = bits << 6 | (digit - digits);
		count += 6;
		if (count >= 8) {
			count -= 8;
			uint32_t c = (bits >> count) & 0xff;

			if (c == 0) {
				break;
			}
			put(out, c);
		}
	}
}

/* The text of a key, "" when it has none. */
static void text_of(const char *obj, const char *key, char *text, size_t size)
{
	struct text out = {.at = text, .end = text + size - 1};
	const char *p = value_of(obj, key);

	if (p != NULL && *p == '"') {
		put_string(&out, p + 1);
	} else if (p != NULL && strncmp(p, "atob(\"", 6) == 0) {
		put_base64(&out, p + 6);
	}
	*out.at = '\0';
	/* Text often ends in line breaks nobody wants to scroll through. */
	while (out.at > text && (out.at[-1] == '\n' || out.at[-1] == ' ')) {
		*--out.at = '\0';
	}
}

/* To the phone */

static void say(const char *json)
{
	char out[160];
	/* Gadgetbridge drops the character before the line break. */
	int n = snprintf(out, sizeof(out), "%s\r\n", json);

	if (n > 0 && n < sizeof(out)) {
		bluetooth_send(out, n);
	}
}

static int last_percent = -1;
static bool last_charging;

void phone_battery(int percent, bool charging, int millivolts)
{
	char json[80];

	if (percent == last_percent && charging == last_charging) {
		return;
	}
	snprintf(json, sizeof(json), "{\"t\":\"status\",\"bat\":%d,\"chg\":%d,\"volt\":%d.%03d}",
		 percent, charging, millivolts / 1000, millivolts % 1000);
	say(json);
	last_percent = percent;
	last_charging = charging;
}

void phone_connected(void)
{
	say("{\"t\":\"ver\",\"fw\":\"" APP_VERSION_STRING "\",\"hw\":\"UNA Watch\"}");
	/* The battery follows with the next reading of it. */
	last_percent = -1;
}

void phone_disconnected(void)
{
	line_len = 0;
	line_too_long = false;
}

/* What the phone says */

static void notify(const char *obj)
{
	static struct phone_note note;
	char more[80];
	int place;

	memset(&note, 0, sizeof(note));
	if (!int_of(obj, "id", &note.id)) {
		return;
	}
	text_of(obj, "src", note.source, sizeof(note.source));
	text_of(obj, "title", note.title, sizeof(note.title));
	if (note.title[0] == '\0') {
		text_of(obj, "sender", note.title, sizeof(note.title));
	}
	/* A mail has a subject besides: it goes first in the body. */
	text_of(obj, "subject", more, sizeof(more));
	if (more[0] != '\0' && strcmp(more, note.title) != 0) {
		size_t n = snprintf(note.body, sizeof(note.body), "%s\n", more);

		text_of(obj, "body", note.body + n, sizeof(note.body) - n);
	} else {
		text_of(obj, "body", note.body, sizeof(note.body));
	}
	note.at = clock_utc();

	k_mutex_lock(&lock, K_FOREVER);
	/* One the watch has already is changed, and comes first again. */
	for (place = 0; place < note_count && notes[place].id != note.id; place++) {
	}
	if (place == note_count) {
		place = MIN(note_count, PHONE_NOTES - 1);
		note_count = MIN(note_count + 1, PHONE_NOTES);
	}
	memmove(&notes[1], &notes[0], place * sizeof(notes[0]));
	notes[0] = note;
	notes_version++;
	k_mutex_unlock(&lock);

	LOG_INF("notification %d from %s", (int)note.id, note.source);
	alert_blip();
	ui_show(&message_screen);
}

static void forget(const char *obj)
{
	int32_t id;

	if (!int_of(obj, "id", &id)) {
		return;
	}
	k_mutex_lock(&lock, K_FOREVER);
	for (int i = 0; i < note_count; i++) {
		if (notes[i].id == id) {
			memmove(&notes[i], &notes[i + 1],
				(note_count - i - 1) * sizeof(notes[0]));
			note_count--;
			notes_version++;
			break;
		}
	}
	k_mutex_unlock(&lock);
	ui_refresh();
}

static void call(const char *obj)
{
	/* alert_ring() keeps the pointer. */
	static char title[48];
	char cmd[12], who[40];

	text_of(obj, "cmd", cmd, sizeof(cmd));
	if (strcmp(cmd, "incoming") != 0) {
		alert_stop();
		return;
	}
	text_of(obj, "name", who, sizeof(who));
	if (who[0] == '\0') {
		text_of(obj, "number", who, sizeof(who));
	}
	snprintf(title, sizeof(title), "%s", who[0] != '\0' ? who : "Call");
	ui_readable(title);
	alert_ring(title, 30);
}

static void find(const char *obj)
{
	int32_t on = 0;

	int_of(obj, "n", &on);
	if (on) {
		alert_ring("Here", 30);
	} else {
		alert_stop();
	}
}

static void set_time(const char *p)
{
	const char *zone = strstr(p, "E.setTimeZone(");
	long long utc = strtoll(p, NULL, 10);
	int offset_min = clock_utc_offset() / 60;

	if (utc < 1600000000) {
		return;
	}
	if (zone != NULL) {
		/* Hours, with a fraction: 1.0, -3.5, 5.75 */
		const char *hours = zone + strlen("E.setTimeZone(");
		bool west = *hours == '-';
		int minutes = abs(atoi(hours)) * 60;
		const char *dot = hours + strspn(hours, "-0123456789");

		if (*dot == '.') {
			int hundredths = 0;

			dot++;
			for (int digit = 0; digit < 2; digit++) {
				hundredths *= 10;
				if (*dot >= '0' && *dot <= '9') {
					hundredths += *dot++ - '0';
				}
			}
			minutes += hundredths * 60 / 100;
		}
		offset_min = west ? -minutes : minutes;
	}
	if (clock_set(utc, offset_min) == 0) {
		status.time_set_at = utc;
		LOG_INF("clock set by the phone, UTC%+d min", offset_min);
	}
}

static void handle_line(char *text)
{
	char type[24];
	char *obj;

	status.lines++;
	if (strncmp(text, "setTime(", 8) == 0) {
		set_time(text + 8);
		return;
	}
	if (strncmp(text, "GB(", 3) != 0 || text[3] != '{') {
		status.unknown++;
		LOG_INF("not understood: %.32s", text);
		return;
	}
	obj = text + 3;
	text_of(obj, "t", type, sizeof(type));
	if (strcmp(type, "notify") == 0) {
		notify(obj);
	} else if (strcmp(type, "notify-") == 0) {
		forget(obj);
	} else if (strcmp(type, "call") == 0) {
		call(obj);
	} else if (strcmp(type, "find") == 0) {
		find(obj);
	} else if (strcmp(type, "is_gps_active") == 0) {
		say("{\"t\":\"gps_power\",\"status\":false}");
	} else {
		/* Music, the volume and the like, many a minute: counted, not logged. */
		status.unknown++;
		LOG_DBG("not understood: type %s", type);
	}
}

void phone_rx(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		char c = data[i];

		if (c == '\n') {
			if (!line_too_long && line_len > 0) {
				line[line_len] = '\0';
				handle_line(line);
			}
			line_len = 0;
			line_too_long = false;
		} else if (c == '\x10' || c == '\r') {
			/* What clears the Bangle.js's input line */
		} else if (line_len < sizeof(line) - 1) {
			line[line_len++] = c;
		} else {
			line_too_long = true;
		}
	}
}

int phone_notes(void)
{
	return note_count;
}

bool phone_note(int index, struct phone_note *note)
{
	bool there;

	k_mutex_lock(&lock, K_FOREVER);
	there = index >= 0 && index < note_count;
	if (there) {
		*note = notes[index];
	}
	k_mutex_unlock(&lock);
	return there;
}

uint32_t phone_notes_version(void)
{
	return notes_version;
}

void phone_status(struct phone_status *out)
{
	*out = status;
}
