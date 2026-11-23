/*
 * A FIT activity file: see fit.h. The layout of the messages follows the
 * stock firmware's files (SDK/Fit/FitProfile.hpp in una-kernel-public), so
 * that what reads those reads these.
 *
 * A FIT file is a 14-byte header, messages, and a CRC over all of it. The
 * header holds the size of the messages, so header and CRC are only right
 * once fit_end has run; fit_resume makes a file whole that never got there.
 */

#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include "fit.h"

#define FIT_EPOCH 631065600 /* 1989-12-31T00:00:00Z */

#define HEADER_SIZE 14

/* Base types */
#define T_ENUM   0x00
#define T_UINT8  0x02
#define T_UINT16 0x84
#define T_SINT32 0x85
#define T_UINT32 0x86
#define T_UINT32Z 0x8c

struct field {
	uint8_t number;
	uint8_t size;
	uint8_t type;
};

struct message {
	uint16_t global;
	uint8_t count;
	const struct field *fields;
};

#define MESSAGE(number, ...)                                                                       \
	{number, sizeof((const struct field[]){__VA_ARGS__}) / sizeof(struct field),                                \
	 (const struct field[]){__VA_ARGS__}}

#define TIMESTAMP {253, 4, T_UINT32}
#define INDEX     {254, 2, T_UINT16}

static const struct message messages[FIT_MESSAGE_COUNT] = {
	/* type (activity), manufacturer, product, serial number, time created */
	[FIT_FILE_ID] = MESSAGE(0, {0, 1, T_ENUM}, {1, 2, T_UINT16}, {2, 2, T_UINT16},
				{3, 4, T_UINT32Z}, {4, 4, T_UINT32}),
	/* event (timer), event type */
	[FIT_EVENT] = MESSAGE(21, TIMESTAMP, {0, 1, T_ENUM}, {1, 1, T_ENUM}),
	/* latitude, longitude, distance, speed, altitude, heart rate */
	[FIT_RECORD] = MESSAGE(20, TIMESTAMP, {0, 4, T_SINT32}, {1, 4, T_SINT32}, {5, 4, T_UINT32},
			       {73, 4, T_UINT32}, {78, 4, T_UINT32}, {3, 1, T_UINT8}),
	/* event (lap), event type (stop), start, elapsed, timer, distance, trigger */
	[FIT_LAP] = MESSAGE(19, INDEX, TIMESTAMP, {0, 1, T_ENUM}, {1, 1, T_ENUM}, {2, 4, T_UINT32},
			    {7, 4, T_UINT32}, {8, 4, T_UINT32}, {9, 4, T_UINT32}, {24, 1, T_ENUM}),
	/*
	 * event (session), event type (stop), start, sport, sub sport, elapsed,
	 * timer, distance, ascent, first lap, laps
	 */
	[FIT_SESSION] = MESSAGE(18, INDEX, TIMESTAMP, {0, 1, T_ENUM}, {1, 1, T_ENUM},
				{2, 4, T_UINT32}, {5, 1, T_ENUM}, {6, 1, T_ENUM}, {7, 4, T_UINT32},
				{8, 4, T_UINT32}, {9, 4, T_UINT32}, {22, 2, T_UINT16},
				{25, 2, T_UINT16}, {26, 2, T_UINT16}),
	/* timer, sessions, type (manual), event (activity), event type (stop), local time */
	[FIT_ACTIVITY] = MESSAGE(34, TIMESTAMP, {0, 4, T_UINT32}, {1, 2, T_UINT16}, {2, 1, T_ENUM},
				 {3, 1, T_ENUM}, {4, 1, T_ENUM}, {5, 4, T_UINT32}),
};

/* Where in a session message the sport is: after its header byte. */
#define SESSION_SPORT_OFFSET (1 + 2 + 4 + 1 + 1 + 4)

static struct fs_file_t file;
static bool is_open;
static uint32_t defined; /* messages whose definition is in the file */
static struct fit_summary summary;

uint32_t fit_time(time_t unix_time)
{
	return unix_time > FIT_EPOCH ? unix_time - FIT_EPOCH : 0;
}

time_t fit_unix_time(uint32_t fit_time)
{
	return (time_t)fit_time + FIT_EPOCH;
}

int32_t fit_semicircles(int64_t nanodegrees)
{
	/* 2^31 / 180 = 11930464.7 */
	return (int32_t)(nanodegrees * 11930465 / 1000000000);
}

uint32_t fit_altitude(int32_t millimetres)
{
	/* A fifth of a metre a step, from 500 m below the sea. */
	return (uint32_t)((millimetres + 500000) / 200);
}

static size_t data_size(enum fit_message message)
{
	size_t size = 1;

	for (int i = 0; i < messages[message].count; i++) {
		size += messages[message].fields[i].size;
	}
	return size;
}

static size_t definition_size(enum fit_message message)
{
	return 6 + 3 * messages[message].count;
}

/* The header, for `size` bytes of messages. */
static void make_header(uint8_t *header, uint32_t size)
{
	header[0] = HEADER_SIZE;
	header[1] = 0x10;                /* protocol 1.0 */
	sys_put_le16(2132, &header[2]);  /* profile 21.32 */
	sys_put_le32(size, &header[4]);
	memcpy(&header[8], ".FIT", 4);
	sys_put_le16(crc16_reflect(0xa001, 0, header, 12), &header[12]);
}

static int write_all(const void *data, size_t size)
{
	ssize_t ret = fs_write(&file, data, size);

	return ret < 0 ? (int)ret : (size_t)ret == size ? 0 : -ENOSPC;
}

/* Keep the summary up with a message that is in the file. */
static void account(enum fit_message message, const uint8_t *data)
{
	if (message == FIT_RECORD) {
		uint32_t distance = sys_get_le32(&data[1 + 12]);

		summary.end = sys_get_le32(&data[1]);
		if (summary.records++ == 0) {
			/* A record closes the second it is of. */
			summary.start = summary.end - 1;
			summary.lap_start = summary.start;
		}
		if (distance != FIT_NONE) {
			summary.distance_cm = distance;
		}
	} else if (message == FIT_LAP) {
		summary.laps++;
		summary.lap_start = summary.end;
		summary.lap_records = summary.records;
		summary.lap_distance_cm = summary.distance_cm;
	}
}

int fit_put(enum fit_message message, const uint32_t *values)
{
	const struct message *m = &messages[message];
	uint8_t buf[64];
	size_t n = 0;
	int ret;

	if (!is_open) {
		return -EBADF;
	}
	if (!(defined & BIT(message))) {
		buf[n++] = 0x40 | message;
		buf[n++] = 0;
		buf[n++] = 0; /* little endian */
		sys_put_le16(m->global, &buf[n]);
		n += 2;
		buf[n++] = m->count;
		for (int i = 0; i < m->count; i++) {
			memcpy(&buf[n], &m->fields[i], 3);
			n += 3;
		}
		ret = write_all(buf, n);
		if (ret < 0) {
			return ret;
		}
		defined |= BIT(message);
		n = 0;
	}
	buf[n++] = message;
	for (int i = 0; i < m->count; i++) {
		uint8_t le[4];

		sys_put_le32(values[i], le);
		memcpy(&buf[n], le, m->fields[i].size);
		n += m->fields[i].size;
	}
	ret = write_all(buf, n);
	if (ret == 0) {
		account(message, buf);
	}
	return ret;
}

int fit_begin(const char *path, uint32_t created)
{
	uint8_t header[HEADER_SIZE];
	int ret;

	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_CREATE | FS_O_TRUNC | FS_O_RDWR);
	if (ret < 0) {
		return ret;
	}
	is_open = true;
	defined = 0;
	summary = (struct fit_summary){0};

	make_header(header, 0);
	ret = write_all(header, sizeof(header));
	if (ret == 0) {
		/* An activity file, by manufacturer 351 (Una), product 1, as stock. */
		ret = fit_put(FIT_FILE_ID, (uint32_t[]){4, 351, 1, 1, created});
	}
	if (ret == 0) {
		ret = fs_sync(&file);
	}
	return ret;
}

/*
 * Go through the messages of a file that was not ended, which are in the
 * order and of the sizes this file writes them, and stop where they do: at
 * the end, or at a message that power was lost in the middle of.
 */
int fit_resume(const char *path)
{
	uint8_t buf[64];
	off_t whole = HEADER_SIZE;
	int ret;

	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_RDWR);
	if (ret < 0) {
		return ret;
	}
	defined = 0;
	summary = (struct fit_summary){0};

	ret = fs_seek(&file, HEADER_SIZE, FS_SEEK_SET);
	while (ret == 0) {
		enum fit_message message;
		size_t size;

		if (fs_read(&file, buf, 1) != 1) {
			break;
		}
		message = buf[0] & 0x0f;
		if (message >= FIT_MESSAGE_COUNT) {
			break;
		}
		if (buf[0] & 0x40) {
			size = definition_size(message);
		} else if (defined & BIT(message)) {
			size = data_size(message);
		} else {
			break;
		}
		if (fs_read(&file, &buf[1], size - 1) != (ssize_t)(size - 1)) {
			break;
		}
		if (buf[0] & 0x40) {
			defined |= BIT(message);
		} else {
			account(message, buf);
		}
		whole += size;
	}
	ret = fs_seek(&file, whole, FS_SEEK_SET);
	if (ret == 0) {
		ret = fs_truncate(&file, whole);
	}
	if (ret < 0) {
		fs_close(&file);
		return ret;
	}
	is_open = true;
	return 0;
}

int fit_sync(void)
{
	return is_open ? fs_sync(&file) : -EBADF;
}

bool fit_is_open(void)
{
	return is_open;
}

const struct fit_summary *fit_summary(void)
{
	return &summary;
}

int fit_lap(bool session_end)
{
	uint32_t elapsed_ms = (summary.end - summary.lap_start) * 1000;
	uint32_t timer_ms = (summary.records - summary.lap_records) * 1000;

	/* Lap event 9, stopped; triggered by distance (2) or the session's end (7). */
	return fit_put(FIT_LAP, (uint32_t[]){summary.laps, summary.end, 9, 1, summary.lap_start,
					     elapsed_ms, timer_ms,
					     summary.distance_cm - summary.lap_distance_cm,
					     session_end ? 7 : 2});
}

/* The CRC that ends the file, over all before it. */
static int write_crc(void)
{
	uint8_t buf[256];
	uint16_t crc = 0;
	ssize_t got;
	int ret = fs_seek(&file, 0, FS_SEEK_SET);

	if (ret < 0) {
		return ret;
	}
	while ((got = fs_read(&file, buf, sizeof(buf))) > 0) {
		crc = crc16_reflect(0xa001, crc, buf, got);
	}
	if (got < 0) {
		return got;
	}
	sys_put_le16(crc, buf);
	return write_all(buf, 2);
}

int fit_end(enum fit_sport sport, uint32_t ascent_m, int32_t utc_offset_s)
{
	uint8_t header[HEADER_SIZE];
	uint32_t elapsed_ms = (summary.end - summary.start) * 1000;
	uint32_t timer_ms = summary.records * 1000;
	off_t size;
	int ret;

	if (!is_open) {
		return -EBADF;
	}
	ret = fit_lap(true);
	if (ret == 0) {
		/* Session event 8, stopped; sub sport generic; laps from the first. */
		ret = fit_put(FIT_SESSION,
			      (uint32_t[]){0, summary.end, 8, 1, summary.start, sport, 0,
					   elapsed_ms, timer_ms, summary.distance_cm, ascent_m, 0,
					   summary.laps});
	}
	if (ret == 0) {
		/* One session, entered by hand (0); activity event 26, stopped. */
		ret = fit_put(FIT_ACTIVITY, (uint32_t[]){summary.end, timer_ms, 1, 0, 26, 1,
							 summary.end + utc_offset_s});
	}
	if (ret == 0) {
		size = fs_tell(&file);
		make_header(header, size - HEADER_SIZE);
		ret = fs_seek(&file, 0, FS_SEEK_SET);
	}
	if (ret == 0) {
		ret = write_all(header, sizeof(header));
	}
	if (ret == 0) {
		ret = write_crc();
	}
	fs_close(&file);
	is_open = false;
	return ret;
}

int fit_set_sport(const char *path, enum fit_sport sport)
{
	/* From the end: the CRC, the activity, and the session before it. */
	off_t back = 2 + data_size(FIT_ACTIVITY) + definition_size(FIT_ACTIVITY) +
		     data_size(FIT_SESSION) - SESSION_SPORT_OFFSET;
	uint8_t byte = sport;
	off_t size;
	int ret;

	if (is_open) {
		return -EBUSY;
	}
	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_RDWR);
	if (ret < 0) {
		return ret;
	}
	ret = fs_seek(&file, 0, FS_SEEK_END);
	if (ret == 0) {
		size = fs_tell(&file);
		ret = fs_seek(&file, size - back, FS_SEEK_SET);
	}
	if (ret == 0) {
		ret = write_all(&byte, 1);
	}
	if (ret == 0) {
		ret = fs_truncate(&file, size - 2);
	}
	if (ret == 0) {
		ret = write_crc();
	}
	fs_close(&file);
	return ret;
}
