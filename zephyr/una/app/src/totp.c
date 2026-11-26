/*
 * Authenticator codes: the secrets, kept in the settings, and the code each
 * gives at a time. SHA-1 and HMAC are here because nothing else in the image
 * has them: the Bluetooth host's cryptography is built without, and the
 * simulator has none.
 *
 * A secret is never printed or logged: the log is kept on the eMMC.
 */
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "clock.h"
#include "totp.h"
#include "ui.h"

#define SLOTS   8
#define KEY_MAX 40 /* 64 characters of base32; less than a block of SHA-1 */

struct slot {
	char name[TOTP_NAME_MAX + 1];
	uint8_t len; /* of the key; 0: empty. The used ones come first. */
	uint8_t key[KEY_MAX];
};

/* The shell changes it, the main thread reads it. */
static K_MUTEX_DEFINE(lock);
static struct slot slots[SLOTS];

#define SHA1_LEN   20
#define SHA1_BLOCK 64

static uint32_t rol(uint32_t x, int n)
{
	return x << n | x >> (32 - n);
}

/* Of a message short enough for what HMAC needs here: a block and a digest. */
static void sha1(const uint8_t *msg, size_t len, uint8_t out[SHA1_LEN])
{
	uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
	uint8_t buf[2 * SHA1_BLOCK] = {0};
	size_t padded = len + 9 <= SHA1_BLOCK ? SHA1_BLOCK : 2 * SHA1_BLOCK;

	__ASSERT_NO_MSG(len + 9 <= sizeof(buf));
	memcpy(buf, msg, len);
	buf[len] = 0x80;
	sys_put_be64((uint64_t)len * 8, &buf[padded - 8]);

	for (size_t at = 0; at < padded; at += SHA1_BLOCK) {
		uint32_t w[80];
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

		for (int i = 0; i < 16; i++) {
			w[i] = sys_get_be32(&buf[at + 4 * i]);
		}
		for (int i = 16; i < 80; i++) {
			w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		}
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;

			if (i < 20) {
				f = (b & c) | (~b & d);
				k = 0x5a827999;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ed9eba1;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8f1bbcdc;
			} else {
				f = b ^ c ^ d;
				k = 0xca62c1d6;
			}
			f += rol(a, 5) + e + k + w[i];
			e = d;
			d = c;
			c = rol(b, 30);
			b = a;
			a = f;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
	}
	for (int i = 0; i < 5; i++) {
		sys_put_be32(h[i], &out[4 * i]);
	}
}

/* RFC 2104, for a key no longer than a block and a message of eight bytes. */
static void hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t msg[8],
		      uint8_t out[SHA1_LEN])
{
	uint8_t inner[SHA1_BLOCK + 8];
	uint8_t outer[SHA1_BLOCK + SHA1_LEN];

	memset(inner, 0x36, SHA1_BLOCK);
	memset(outer, 0x5c, SHA1_BLOCK);
	for (size_t i = 0; i < key_len; i++) {
		inner[i] ^= key[i];
		outer[i] ^= key[i];
	}
	memcpy(&inner[SHA1_BLOCK], msg, 8);
	sha1(inner, sizeof(inner), &outer[SHA1_BLOCK]);
	sha1(outer, sizeof(outer), out);
}

/* RFC 4226's six digits from RFC 6238's count of steps. */
static uint32_t code_at(const struct slot *slot, time_t utc)
{
	uint8_t count[8];
	uint8_t mac[SHA1_LEN];

	sys_put_be64((uint64_t)utc / TOTP_STEP_S, count);
	hmac_sha1(slot->key, slot->len, count, mac);
	return (sys_get_be32(&mac[mac[SHA1_LEN - 1] & 0x0f]) & 0x7fffffff) % 1000000;
}

/*
 * Base32 as the services show it: either case, in groups with spaces or
 * dashes, with or without the padding. The number of bytes, or -1.
 */
static int base32_decode(const char *in, uint8_t *out, size_t size)
{
	uint32_t bits = 0;
	int have = 0;
	size_t len = 0;

	for (; *in != '\0'; in++) {
		char c = *in;
		int value;

		if (c == ' ' || c == '-' || c == '=') {
			continue;
		}
		if (c >= 'a' && c <= 'z') {
			c -= 'a' - 'A';
		}
		if (c >= 'A' && c <= 'Z') {
			value = c - 'A';
		} else if (c >= '2' && c <= '7') {
			value = c - '2' + 26;
		} else {
			return -1;
		}
		bits = bits << 5 | value;
		have += 5;
		if (have >= 8) {
			if (len == size) {
				return -1;
			}
			have -= 8;
			out[len++] = bits >> have;
		}
	}
	return len;
}

/* With the lock held. */
static int count_locked(void)
{
	int n = 0;

	while (n < SLOTS && slots[n].len != 0) {
		n++;
	}
	return n;
}

static int find_locked(const char *name)
{
	for (int i = 0; i < count_locked(); i++) {
		if (strcmp(slots[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}

int totp_count(void)
{
	int n;

	k_mutex_lock(&lock, K_FOREVER);
	n = count_locked();
	k_mutex_unlock(&lock);
	return n;
}

bool totp_get(int index, time_t utc, char name[TOTP_NAME_MAX + 1], uint32_t *code)
{
	bool found;

	k_mutex_lock(&lock, K_FOREVER);
	found = index >= 0 && index < count_locked();
	if (found) {
		strcpy(name, slots[index].name);
		*code = code_at(&slots[index], utc);
	}
	k_mutex_unlock(&lock);
	return found;
}

static int totp_setting_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "table", NULL) && len == sizeof(slots)) {
		int err = read_cb(cb_arg, slots, sizeof(slots)) < 0 ? -EIO : 0;

		/* Whatever was stored, a name ends and a key fits. */
		for (int i = 0; i < SLOTS; i++) {
			slots[i].name[TOTP_NAME_MAX] = '\0';
			slots[i].len = MIN(slots[i].len, KEY_MAX);
		}
		return err;
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(totp, "totp", NULL, totp_setting_set, NULL, NULL);

/* With the lock held. */
static int save_locked(void)
{
	return settings_save_one("totp/table", slots, sizeof(slots));
}

static int cmd_add(const struct shell *sh, size_t argc, char **argv)
{
	const char *name = argv[1];
	struct slot slot = {0};
	int len = base32_decode(argv[2], slot.key, sizeof(slot.key));
	int err = 0;
	int n;

	if (strlen(name) > TOTP_NAME_MAX) {
		shell_error(sh, "a name is at most %d characters", TOTP_NAME_MAX);
		return -EINVAL;
	}
	for (const char *c = name; *c != '\0'; c++) {
		if (*c < 0x21 || *c > 0x7e) {
			shell_error(sh, "a name is of plain letters, digits and signs");
			return -EINVAL;
		}
	}
	if (len <= 0) {
		shell_error(sh, "not a secret: base32 (A-Z, 2-7), at most %d characters",
			    KEY_MAX * 8 / 5);
		return -EINVAL;
	}
	strcpy(slot.name, name);
	slot.len = len;

	k_mutex_lock(&lock, K_FOREVER);
	n = count_locked();
	if (find_locked(name) >= 0) {
		shell_error(sh, "there is a %s already", name);
		err = -EEXIST;
	} else if (n == SLOTS) {
		shell_error(sh, "there is room for %d", SLOTS);
		err = -ENOSPC;
	} else {
		slots[n] = slot;
		err = save_locked();
		if (err != 0) {
			shell_error(sh, "not stored: %d", err);
		}
	}
	k_mutex_unlock(&lock);
	ui_refresh();
	return err;
}

static int cmd_del(const struct shell *sh, size_t argc, char **argv)
{
	int err = 0;
	int at;

	k_mutex_lock(&lock, K_FOREVER);
	at = find_locked(argv[1]);
	if (at < 0) {
		shell_error(sh, "there is no %s", argv[1]);
		err = -ENOENT;
	} else {
		/* The ones after it move up; nothing of the last is left behind. */
		memmove(&slots[at], &slots[at + 1], (SLOTS - 1 - at) * sizeof(slots[0]));
		memset(&slots[SLOTS - 1], 0, sizeof(slots[0]));
		err = save_locked();
		if (err != 0) {
			shell_error(sh, "not stored: %d", err);
		}
	}
	k_mutex_unlock(&lock);
	ui_refresh();
	return err;
}

static int cmd_list(const struct shell *sh, size_t argc, char **argv)
{
	k_mutex_lock(&lock, K_FOREVER);
	for (int i = 0; i < count_locked(); i++) {
		shell_print(sh, "%s", slots[i].name);
	}
	k_mutex_unlock(&lock);
	return 0;
}

static int cmd_code(const struct shell *sh, size_t argc, char **argv)
{
	time_t utc = argc > 2 ? (time_t)strtoll(argv[2], NULL, 10) : clock_utc();
	uint32_t code = 0;
	int at;

	if (utc == 0) {
		shell_error(sh, "the clock is not set");
		return -ENODATA;
	}
	k_mutex_lock(&lock, K_FOREVER);
	at = find_locked(argv[1]);
	if (at >= 0) {
		code = code_at(&slots[at], utc);
	}
	k_mutex_unlock(&lock);
	if (at < 0) {
		shell_error(sh, "there is no %s", argv[1]);
		return -ENOENT;
	}
	shell_print(sh, "%06u", code);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(totp_cmds,
	SHELL_CMD_ARG(add, NULL, "add <name> <secret in base32>", cmd_add, 3, 0),
	SHELL_CMD_ARG(del, NULL, "del <name>", cmd_del, 2, 0),
	SHELL_CMD_ARG(list, NULL, "The names", cmd_list, 1, 0),
	SHELL_CMD_ARG(code, NULL, "code <name> [<seconds since 1970 UTC>]", cmd_code, 2, 1),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(totp, &totp_cmds, "Authenticator codes", NULL);
