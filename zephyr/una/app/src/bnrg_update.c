/*
 * Loads ST's firmware into the BlueNRG-2N, in place of the radio (see
 * bnrg-update.conf). The chip is reached over SPI only, so the image goes in
 * through the updater it carries in its first 8 KB: erase the flag that marks
 * the stack image as whole, write the image sector by sector, have the chip
 * sum each sector, and set the flag again only when every sum is right.
 * Without the flag the chip starts as its updater, so a run that was cut
 * short is simply run again.
 *
 * As ST's own does it: x-cube-ble2, IFRStack_Updater/Src/bluenrg_2_utils.c.
 * The sector that holds the board's settings (crystal, low-speed clock, power
 * supply) is left alone by the update. "bnrg smps" writes one byte of it:
 * whether the chip is to run its switching regulator, which a board without
 * the inductor for it, as the watch's is, should have off.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "bluenrg.h"
#include "bluetooth.h"
#include "power.h"

static const uint8_t image[] = {
#include <bnrg_fw.inc>
};

#define FIRMWARE_DETAILS BLUENRG_ACI(0x001)
#define UPDATER_START    BLUENRG_ACI(0x020)
#define UPDATER_VERSION  BLUENRG_ACI(0x022)
#define ERASE_BLUE_FLAG  BLUENRG_ACI(0x024)
#define RESET_BLUE_FLAG  BLUENRG_ACI(0x025)
#define ERASE_SECTOR     BLUENRG_ACI(0x026)
#define PROGRAM_BLOCK    BLUENRG_ACI(0x027)
#define READ_BLOCK       BLUENRG_ACI(0x028)
#define CALC_CRC         BLUENRG_ACI(0x029)

#define VENDOR_INITIALIZED 0x0001
#define STARTED_STACK      1
#define STARTED_UPDATER(r) ((r) >= 2 && (r) <= 4)

#define CHIP_FLASH     0x10040000
#define CHIP_FLASH_END 0x10080000
#define IMAGE_BASE     (CHIP_FLASH + 8 * 1024) /* after the updater */
#define SECTOR       2048
#define BLOCK        64
/* The board's settings, in the last sector. */
#define DEV_CONFIG   (CHIP_FLASH_END - SECTOR + 0x100)
#define DEV_CONFIG_N 14
/* Of them, the third: 0xff the regulator with 10 uH, 0x00 with 4.7 uH, 0x01 off. */
#define DEV_CONFIG_SMPS 2
#define SMPS_10UH       0xff
#define SMPS_OFF        0x01

#define MIN_BATTERY 30

/* The radio there is not, for the rest of the watch. */

void bluetooth_set(bool on)
{
	ARG_UNUSED(on);
}

bool bluetooth_is_set(void)
{
	return false;
}

void bluetooth_forget(void)
{
}

void bluetooth_status(struct bluetooth_status *status)
{
	*status = (struct bluetooth_status){.state = BLUETOOTH_OFF};
}

bool bluetooth_send(const void *data, size_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	return false;
}

/* A command, once more when no answer came. */
static int cmd(uint16_t opcode, const void *params, size_t len, void *rsp, size_t rsp_size)
{
	int ret = bluenrg_cmd(opcode, params, len, rsp, rsp_size);

	if (ret < 0) {
		k_msleep(50);
		ret = bluenrg_cmd(opcode, params, len, rsp, rsp_size);
	}
	return ret;
}

/* Out of reset. Says what the chip started as; 0 when it did not. */
static uint8_t start(const struct shell *sh)
{
	int ret = bluenrg_start();

	if (ret != 0) {
		shell_error(sh, "the chip did not start: %d", ret);
		return 0;
	}
	return bluenrg_start_reason();
}

static bool show_firmware(const struct shell *sh, bool *current)
{
	uint8_t rsp[14] = {0};
	uint16_t variant;

	if (cmd(FIRMWARE_DETAILS, NULL, 0, rsp, sizeof(rsp)) != 0) {
		shell_error(sh, "the chip does not say what firmware it has");
		return false;
	}
	variant = sys_get_le16(&rsp[10]);
	shell_print(sh, "firmware %u.%u.%u (%s), stack %u.%u%c, variant 0x%04x%s", rsp[0], rsp[1],
		    rsp[2], rsp[3] == 2 ? "SPI" : rsp[3] == 1 ? "UART" : "?", rsp[6], rsp[7],
		    'a' + rsp[8] - 1, variant, (variant & 0x10) != 0 ? " (link layer only)" : "");
	if (current != NULL) {
		/* The image says nothing of its version: this is the one it is used for. */
		*current = rsp[0] == 3 && rsp[1] == 4 && rsp[2] == 0;
	}
	return true;
}

/* Into the updater, from whatever the chip started as. */
static bool enter_updater(const struct shell *sh)
{
	struct bluenrg_event ev;
	uint8_t reason = start(sh);
	uint8_t version = 0;
	int64_t until;

	if (reason == 0) {
		return false;
	}
	if (!STARTED_UPDATER(reason)) {
		/* It restarts rather than answers, or answers and restarts. */
		(void)bluenrg_cmd(UPDATER_START, NULL, 0, NULL, 0);
		reason = 0;
		until = k_uptime_get() + 5000;
		while (k_uptime_get() < until) {
			if (bluenrg_event(&ev, K_MSEC(200)) && ev.event == BLUENRG_EVENT_VENDOR &&
			    ev.code == VENDOR_INITIALIZED && ev.len >= 1) {
				reason = ev.data[0];
				break;
			}
		}
		if (!STARTED_UPDATER(reason)) {
			shell_error(sh, "the chip did not enter its updater (said %u)", reason);
			return false;
		}
	}
	if (cmd(UPDATER_VERSION, NULL, 0, &version, 1) != 0) {
		shell_error(sh, "the updater does not answer");
		return false;
	}
	shell_print(sh, "in the updater, version %u (started as %u)", version, reason);
	return true;
}

static bool read_dev_config(const struct shell *sh, uint8_t *config)
{
	uint8_t params[6];

	sys_put_le32(DEV_CONFIG, &params[0]);
	sys_put_le16(DEV_CONFIG_N, &params[4]);
	if (cmd(READ_BLOCK, params, sizeof(params), config, DEV_CONFIG_N) != 0) {
		shell_error(sh, "the board's settings could not be read");
		return false;
	}
	shell_fprintf(sh, SHELL_NORMAL, "board settings:");
	for (int i = 0; i < DEV_CONFIG_N; i++) {
		shell_fprintf(sh, SHELL_NORMAL, " %02x", config[i]);
	}
	shell_fprintf(sh, SHELL_NORMAL, "\n");
	return true;
}

/* The sum the updater makes of a sector, of what the sector should hold. */
static uint32_t sector_crc(const uint8_t *data, size_t len)
{
	uint32_t crc = 0;

	for (size_t i = 0; i < SECTOR; i += 4) {
		uint8_t word[4];

		for (size_t j = 0; j < 4; j++) {
			word[j] = i + j < len ? data[i + j] : 0xff;
		}
		crc ^= sys_get_le32(word);
		for (int bit = 0; bit < 32; bit++) {
			crc = (crc << 1) ^ ((crc & BIT(31)) != 0 ? 0x04c11db7 : 0);
		}
	}
	return crc;
}

static bool write_sector(const struct shell *sh, size_t at)
{
	uint8_t params[6 + BLOCK];
	size_t end = MIN(at + SECTOR, sizeof(image));
	int ret;

	sys_put_le32(IMAGE_BASE + at, params);
	ret = cmd(ERASE_SECTOR, params, 4, NULL, 0);
	for (size_t i = at; ret == 0 && i < end; i += BLOCK) {
		/* Whole words: what the image lacks of the last one stays erased. */
		size_t have = MIN(BLOCK, end - i);
		size_t len = ROUND_UP(have, 4);

		sys_put_le32(IMAGE_BASE + i, &params[0]);
		sys_put_le16(len, &params[4]);
		memset(&params[6], 0xff, len);
		memcpy(&params[6], &image[i], have);
		ret = cmd(PROGRAM_BLOCK, params, 6 + len, NULL, 0);
	}
	if (ret != 0) {
		shell_error(sh, "sector at 0x%05zx: %d", at, ret);
	}
	return ret == 0;
}

static bool check_sector(const struct shell *sh, size_t at)
{
	uint8_t params[5], rsp[4] = {0};
	uint32_t want = sector_crc(&image[at], MIN(SECTOR, sizeof(image) - at));

	sys_put_le32(IMAGE_BASE + at, params);
	params[4] = 1;
	if (cmd(CALC_CRC, params, sizeof(params), rsp, sizeof(rsp)) != 0) {
		shell_error(sh, "sector at 0x%05zx: no sum", at);
		return false;
	}
	if (sys_get_le32(rsp) != want) {
		shell_error(sh, "sector at 0x%05zx: sum 0x%08x, should be 0x%08x", at,
			    sys_get_le32(rsp), want);
		return false;
	}
	return true;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t config[DEV_CONFIG_N];
	uint8_t reason = start(sh);
	bool ok = reason != 0;

	if (reason == STARTED_STACK) {
		ok = show_firmware(sh, NULL);
	} else if (reason != 0) {
		shell_print(sh, "the chip started as %u, not as the stack", reason);
	}
	bluenrg_stop();
	ok = ok && enter_updater(sh) && read_dev_config(sh, config);
	bluenrg_stop();
	shell_print(sh, "image to load: %zu bytes", sizeof(image));
	return ok ? 0 : -EIO;
}

/* bnrg read <address> <bytes>, both in hex: the chip's flash, 32 bytes a line. */
static int cmd_read(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t at = strtoul(argv[1], NULL, 16);
	uint32_t end = at + strtoul(argv[2], NULL, 16);
	uint8_t params[6], data[32];
	char line[2 * sizeof(data) + 1];
	bool ok = enter_updater(sh);

	while (ok && at < end) {
		size_t len = MIN(sizeof(data), end - at);

		sys_put_le32(at, &params[0]);
		sys_put_le16(len, &params[4]);
		ok = cmd(READ_BLOCK, params, sizeof(params), data, len) == 0;
		if (ok) {
			bin2hex(data, len, line, sizeof(line));
			shell_print(sh, "%08x %s", at, line);
		}
		at += len;
	}
	bluenrg_stop();
	return ok ? 0 : -EIO;
}

/* bnrg update [again]: again writes it even when the chip says it has it. */
static int cmd_update(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t before[DEV_CONFIG_N], after[DEV_CONFIG_N];
	struct power_status power;
	bool current = false, ok = false;
	uint8_t reason;
	size_t at;

	power_last(&power);
	if (!power.usb_power && power.percent < MIN_BATTERY) {
		shell_error(sh, "battery at %d%%: charge it first", power.percent);
		return -EAGAIN;
	}
	if (sizeof(image) > CHIP_FLASH_END - SECTOR - IMAGE_BASE || sys_get_le32(&image[4]) < IMAGE_BASE ||
	    sys_get_le32(&image[4]) >= CHIP_FLASH_END) {
		shell_error(sh, "that is not an image for this chip");
		return -EINVAL;
	}
	reason = start(sh);
	if (reason == STARTED_STACK && show_firmware(sh, &current) && current && argc < 2) {
		shell_print(sh, "nothing to do (\"bnrg update again\" writes it anyway)");
		bluenrg_stop();
		return 0;
	}
	bluenrg_stop();
	if (!enter_updater(sh) || !read_dev_config(sh, before)) {
		goto out;
	}
	if (cmd(ERASE_BLUE_FLAG, NULL, 0, NULL, 0) != 0) {
		shell_error(sh, "the image's flag could not be erased; nothing is changed");
		goto out;
	}
	shell_print(sh, "writing %zu bytes", sizeof(image));
	for (at = 0; at < sizeof(image); at += SECTOR) {
		if (!write_sector(sh, at)) {
			goto cut_short;
		}
		if ((at / SECTOR) % 8 == 7) {
			shell_print(sh, "%zu%%", at * 100 / sizeof(image));
		}
	}
	shell_print(sh, "checking");
	for (at = 0; at < sizeof(image); at += SECTOR) {
		if (!check_sector(sh, at)) {
			goto cut_short;
		}
	}
	if (cmd(RESET_BLUE_FLAG, NULL, 0, NULL, 0) != 0) {
		shell_error(sh, "the image's flag could not be set");
		goto cut_short;
	}
	if (read_dev_config(sh, after) && memcmp(before, after, sizeof(before)) != 0) {
		shell_warn(sh, "the board's settings changed, which nothing here does");
	}
	bluenrg_stop();
	reason = start(sh);
	if (reason != STARTED_STACK) {
		shell_error(sh, "written and checked, but the chip started as %u", reason);
		goto out;
	}
	ok = show_firmware(sh, NULL);
	goto out;

cut_short:
	shell_error(sh, "the chip has no whole image now and stays in its updater: "
			"run \"bnrg update\" again");
out:
	bluenrg_stop();
	return ok ? 0 : -EIO;
}

/*
 * bnrg smps on|off: the settings sector read whole, the one byte changed,
 * the sector erased, written and read again. "on" is the byte as it is in a
 * chip nobody has set up (10 uH), which is how the watch came.
 */
static int cmd_smps(const struct shell *sh, size_t argc, char **argv)
{
	static uint8_t sector[SECTOR], check[SECTOR];
	const uint32_t base = CHIP_FLASH_END - SECTOR;
	const size_t at = DEV_CONFIG - base + DEV_CONFIG_SMPS;
	uint8_t params[6 + BLOCK], value, reason;
	struct power_status power;
	bool ok = false;
	int ret = 0;

	if (strcmp(argv[1], "off") == 0) {
		value = SMPS_OFF;
	} else if (strcmp(argv[1], "on") == 0) {
		value = SMPS_10UH;
	} else {
		shell_error(sh, "bnrg smps on|off");
		return -EINVAL;
	}
	power_last(&power);
	if (!power.usb_power && power.percent < MIN_BATTERY) {
		shell_error(sh, "battery at %d%%: charge it first", power.percent);
		return -EAGAIN;
	}
	if (!enter_updater(sh)) {
		goto out;
	}
	for (size_t i = 0; ret == 0 && i < SECTOR; i += BLOCK) {
		sys_put_le32(base + i, &params[0]);
		sys_put_le16(BLOCK, &params[4]);
		ret = cmd(READ_BLOCK, params, 6, &sector[i], BLOCK);
	}
	if (ret != 0) {
		shell_error(sh, "the settings could not be read; nothing is changed");
		goto out;
	}
	shell_print(sh, "the byte is 0x%02x", sector[at]);
	if (sector[at] == value) {
		shell_print(sh, "nothing to do");
		ok = true;
		goto out;
	}
	sector[at] = value;
	sys_put_le32(base, params);
	ret = cmd(ERASE_SECTOR, params, 4, NULL, 0);
	for (size_t i = 0; ret == 0 && i < SECTOR; i += BLOCK) {
		bool blank = true;

		for (size_t j = 0; j < BLOCK; j++) {
			blank = blank && sector[i + j] == 0xff;
		}
		if (blank) {
			continue;
		}
		sys_put_le32(base + i, &params[0]);
		sys_put_le16(BLOCK, &params[4]);
		memcpy(&params[6], &sector[i], BLOCK);
		ret = cmd(PROGRAM_BLOCK, params, sizeof(params), NULL, 0);
	}
	for (size_t i = 0; ret == 0 && i < SECTOR; i += BLOCK) {
		sys_put_le32(base + i, &params[0]);
		sys_put_le16(BLOCK, &params[4]);
		ret = cmd(READ_BLOCK, params, 6, &check[i], BLOCK);
	}
	if (ret != 0 || memcmp(sector, check, SECTOR) != 0) {
		shell_error(sh, "the settings are not as they were meant to be (%d): run it again",
			    ret);
		goto out;
	}
	shell_print(sh, "written: the byte is 0x%02x", check[at]);
	bluenrg_stop();
	reason = start(sh);
	if (reason != STARTED_STACK) {
		shell_error(sh, "the chip started as %u, not as the stack", reason);
		goto out;
	}
	ok = show_firmware(sh, NULL);
out:
	bluenrg_stop();
	return ok ? 0 : -EIO;
}

SHELL_STATIC_SUBCMD_SET_CREATE(bnrg_cmds,
	SHELL_CMD(info, NULL, "the chip's firmware and the board's settings in it", cmd_info),
	SHELL_CMD_ARG(read, NULL, "<address> <bytes>, in hex: the chip's flash", cmd_read, 3, 0),
	SHELL_CMD_ARG(update, NULL, "[again]: write ST's image into the chip", cmd_update, 1, 1),
	SHELL_CMD_ARG(smps, NULL, "on|off: the chip's switching regulator, in its settings",
		      cmd_smps, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(bnrg, &bnrg_cmds, "The radio chip's firmware", NULL);
