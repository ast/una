/*
 * MS5837-02BA on I2C2, its supply switched by a pin.
 *
 * The sensor is asked to convert, pressure and then temperature, and each
 * is read when the conversion is over. What comes out is raw: the sensor
 * carries the figures of its own calibration, read once after power-on, and
 * the datasheet gives the arithmetic that makes Pa and degrees of them.
 *
 * Zephyr has a driver for the part. It is not used: it reads the calibration
 * at boot, when this one has no supply, and without the pause the sensor
 * needs after a reset or a check of what was read.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "baro.h"

LOG_MODULE_REGISTER(baro_hw, LOG_LEVEL_INF);

static const struct device *const bus = DEVICE_DT_GET(DT_NODELABEL(i2c2));
static const struct gpio_dt_spec enable = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), baro_en_gpios);

#define ADDRESS 0x76

#define CMD_RESET       0x1e
#define CMD_READ        0x00
#define CMD_PRESSURE    0x4a /* 8192 samples to a reading: the least noise */
#define CMD_TEMPERATURE 0x54 /* 1024 */
#define CMD_PROM        0xa0

/* As the stock firmware waits; the datasheet's figures are below these. */
#define POWER_OFF_MS       20
#define POWER_ON_MS        10
#define RESET_MS           10
#define PRESSURE_MS        20
#define TEMPERATURE_MS     3
#define ATTEMPTS           3

#define PROM_WORDS 7

static uint16_t prom[PROM_WORDS];

/* The checksum over the calibration, four bits of its first word. */
static uint8_t crc4(const uint16_t *words)
{
	uint16_t w[8];
	uint16_t rem = 0;

	memcpy(w, words, PROM_WORDS * sizeof(uint16_t));
	w[0] &= 0x0fff;
	w[7] = 0;
	for (int i = 0; i < 16; i++) {
		rem ^= i % 2 == 1 ? w[i / 2] & 0xff : w[i / 2] >> 8;
		for (int bit = 0; bit < 8; bit++) {
			rem = rem & 0x8000 ? (rem << 1) ^ 0x3000 : rem << 1;
		}
	}
	return rem >> 12;
}

static int start(void)
{
	uint8_t cmd = CMD_RESET;
	bool blank = true;
	int ret;

	ret = i2c_write(bus, &cmd, 1, ADDRESS);
	if (ret != 0) {
		return ret;
	}
	k_msleep(RESET_MS);
	for (int i = 0; i < PROM_WORDS; i++) {
		uint8_t raw[2];

		ret = i2c_burst_read(bus, ADDRESS, CMD_PROM + 2 * i, raw, sizeof(raw));
		if (ret != 0) {
			return ret;
		}
		prom[i] = sys_get_be16(raw);
		blank = blank && (prom[i] == 0 || prom[i] == 0xffff);
	}
	/* All zeros pass the checksum and are no calibration. */
	if (blank || crc4(prom) != prom[0] >> 12) {
		return -EBADMSG;
	}
	return 0;
}

int baro_hw_power(bool on)
{
	static bool told;
	int ret = -ENODEV;

	if (!device_is_ready(bus) || !gpio_is_ready_dt(&enable)) {
		return -ENODEV;
	}
	/* Off first, whatever it was: a sensor that is stuck needs it. */
	gpio_pin_configure_dt(&enable, GPIO_OUTPUT_INACTIVE);
	if (!on) {
		return 0;
	}
	for (int attempt = 0; attempt < ATTEMPTS && ret != 0; attempt++) {
		gpio_pin_set_dt(&enable, 0);
		k_msleep(POWER_OFF_MS);
		gpio_pin_set_dt(&enable, 1);
		k_msleep(POWER_ON_MS);
		ret = start();
	}
	if (ret == 0 && !told) {
		told = true;
		/* 0 is the 02BA01, which the arithmetic below is for. */
		LOG_INF("MS5837, version code 0x%02x, calibration %u %u %u %u %u %u",
			(prom[0] >> 5) & 0x7f, prom[1], prom[2], prom[3], prom[4], prom[5],
			prom[6]);
	}
	return ret;
}

static int convert(uint8_t cmd, int wait_ms, uint32_t *raw)
{
	uint8_t bytes[3];
	int ret;

	ret = i2c_write(bus, &cmd, 1, ADDRESS);
	if (ret != 0) {
		return ret;
	}
	k_msleep(wait_ms);
	ret = i2c_burst_read(bus, ADDRESS, CMD_READ, bytes, sizeof(bytes));
	*raw = sys_get_be24(bytes);
	/* A conversion not over reads as nothing. */
	return ret != 0 ? ret : *raw == 0 ? -EAGAIN : 0;
}

int baro_hw_sample(int32_t *pressure_pa, int32_t *temperature_cc)
{
	uint32_t d1, d2;
	int64_t dt, off, sens, temp;
	int ret;

	ret = convert(CMD_PRESSURE, PRESSURE_MS, &d1);
	if (ret == 0) {
		ret = convert(CMD_TEMPERATURE, TEMPERATURE_MS, &d2);
	}
	if (ret != 0) {
		return ret;
	}
	/* The datasheet's, for the 02BA: first order, then the correction in the cold. */
	dt = (int64_t)d2 - ((int64_t)prom[5] << 8);
	temp = 2000 + dt * prom[6] / (1LL << 23);
	off = ((int64_t)prom[2] << 17) + dt * prom[4] / (1LL << 6);
	sens = ((int64_t)prom[1] << 16) + dt * prom[3] / (1LL << 7);
	if (temp < 2000) {
		int64_t below = (temp - 2000) * (temp - 2000);

		temp -= 11 * dt * dt / (1LL << 35);
		off -= 31 * below / (1LL << 3);
		sens -= 63 * below / (1LL << 5);
	}
	/* In hundredths of a millibar, which are Pa. */
	*pressure_pa = (int32_t)((sens * d1 / (1LL << 21) - off) / (1LL << 15));
	*temperature_cc = (int32_t)temp;
	return 0;
}
