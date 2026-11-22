/*
 * Copyright (c) 2021 Bosch Sensortec GmbH
 * Copyright (c) 2022 Nordic Semiconductor ASA
 * Copyright (c) 2026 Infineon Technologies AG,
 * or an affiliate of Infineon Technologies AG.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Bosch BMI270 on I2C, for a watch.
 *
 * Zephyr's driver (drivers/sensor/bosch/bmi270), with what a wrist needs
 * of the chip added and what it does not need left out:
 *
 * - The step counter and detector, the activity, the wrist-wear wake-up
 *   and no motion, besides any motion: see <una/bmi270.h>. The
 *   accelerometer runs at the 50 Hz they need while one of them is on, and
 *   is off again with the last.
 * - The feature registers are written with advanced power save off, as
 *   the datasheet has it; with it on, as there, the writes are not sure to
 *   arrive. Every other write is followed by the 450 us that mode asks for.
 * - One interrupt line, wired to either pin of the chip (int-pin), carries
 *   every interrupt. There, one line is INT1 and has any motion alone.
 * - The mounting is in the devicetree (axes): the features are told,
 *   and the readings come out in the device's frame.
 * - The chip is left suspended, at a few microamps, until something is
 *   asked of it: the temperature sensor is not on all the time.
 * - The configuration goes up in pieces of 256 bytes, not 32 with a
 *   millisecond between each, and the node may be zephyr,deferred-init.
 * - SPI, the FIFO and its streaming are left out.
 */

#define DT_DRV_COMPAT una_bmi270

#include <zephyr/drivers/sensor.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <una/bmi270.h>

#include "bmi270.h"
#include "bmi270_config_file.h"

LOG_MODULE_REGISTER(una_bmi270, CONFIG_SENSOR_LOG_LEVEL);

int bmi270_reg_read(const struct device *dev, uint8_t reg, uint8_t *data, uint16_t length)
{
	const struct bmi270_config *cfg = dev->config;

	return i2c_burst_read_dt(&cfg->i2c, reg, data, length);
}

/* For any mode of the chip: the wait is the longest one. */
int bmi270_reg_write(const struct device *dev, uint8_t reg, uint8_t value)
{
	const struct bmi270_config *cfg = dev->config;
	int ret = i2c_reg_write_byte_dt(&cfg->i2c, reg, value);

	k_usleep(BMI270_WRITE_IDLE_SUSPEND);
	return ret;
}

int bmi270_feat_begin(const struct device *dev)
{
	return bmi270_reg_write(dev, BMI270_REG_PWR_CONF, BMI270_PWR_CONF_ADV_PWR_SAVE_DIS);
}

int bmi270_feat_end(const struct device *dev)
{
	return bmi270_reg_write(dev, BMI270_REG_PWR_CONF, BMI270_PWR_CONF_ADV_PWR_SAVE_EN);
}

static int feat_page(const struct device *dev, uint8_t page)
{
	const struct bmi270_config *cfg = dev->config;

	return i2c_reg_write_byte_dt(&cfg->i2c, BMI270_REG_FEAT_PAGE, page);
}

int bmi270_feat_read(const struct device *dev, uint8_t page, uint8_t reg, uint16_t *value)
{
	uint8_t raw[2];
	int ret = feat_page(dev, page);

	if (ret == 0) {
		ret = bmi270_reg_read(dev, reg, raw, sizeof(raw));
	}
	*value = sys_get_le16(raw);
	return ret;
}

int bmi270_feat_write(const struct device *dev, uint8_t page, uint8_t reg, uint16_t value)
{
	const struct bmi270_config *cfg = dev->config;
	uint8_t raw[2];
	int ret = feat_page(dev, page);

	sys_put_le16(value, raw);
	if (ret == 0) {
		ret = i2c_burst_write_dt(&cfg->i2c, reg, raw, sizeof(raw));
	}
	LOG_DBG("feature reg[0x%02x]@%d = 0x%04x: %d", reg, page, value, ret);
	return ret;
}

int bmi270_feat_update(const struct device *dev, uint8_t page, uint8_t reg, uint16_t mask,
		       uint16_t value)
{
	uint16_t old;
	int ret = bmi270_feat_read(dev, page, reg, &old);

	if (ret != 0) {
		return ret;
	}
	return bmi270_feat_write(dev, page, reg, (old & ~mask) | (value & mask));
}

/*
 * The registers that say what runs and how, written from what has been
 * asked for. The features have the accelerometer at 50 Hz at the least.
 */
static int apply(const struct device *dev)
{
	struct bmi270_data *data = dev->data;
	bool low_power = IS_ENABLED(CONFIG_UNA_BMI270_LOW_POWER_MODE);
	uint8_t acc_odr = data->acc_odr;
	uint8_t pwr_ctrl = 0;
	int ret = 0;

	if (data->features != 0 && acc_odr < BMI270_ACC_ODR_50_HZ) {
		acc_odr = BMI270_ACC_ODR_50_HZ;
	}
	if (acc_odr != 0) {
		uint8_t conf = acc_odr | data->acc_bwp << BMI270_ACC_BWP_POS;

		if (!low_power && acc_odr >= BMI270_ACC_ODR_100_HZ) {
			conf |= BMI270_ACC_FILT_PERF_OPT;
		}
		pwr_ctrl |= BMI270_PWR_CTRL_ACC_EN;
		ret = bmi270_reg_write(dev, BMI270_REG_ACC_CONF, conf);
	}
	if (ret == 0 && data->gyr_odr != 0) {
		uint8_t conf = data->gyr_odr | data->gyr_bwp << BMI270_GYR_BWP_POS;

		if (!low_power && data->gyr_odr >= BMI270_GYR_ODR_100_HZ) {
			conf |= BMI270_GYR_FILT_PERF_OPT | BMI270_GYR_FILT_NOISE_PERF;
		}
		pwr_ctrl |= BMI270_PWR_CTRL_GYR_EN;
		ret = bmi270_reg_write(dev, BMI270_REG_GYR_CONF, conf);
	}
	/* The temperature for whoever reads the sensors, not for the features. */
	if (data->acc_odr != 0 || data->gyr_odr != 0) {
		pwr_ctrl |= BMI270_PWR_CTRL_TEMP_EN;
	}
	if (ret == 0) {
		ret = bmi270_reg_write(dev, BMI270_REG_PWR_CTRL, pwr_ctrl);
	}
	return ret;
}

static void channel_accel_convert(struct sensor_value *val, int64_t raw_val, uint8_t range)
{
	/* 16 bit accelerometer. 2^15 bits represent the range in G */
	/* Converting from G to m/s^2 */
	raw_val = (raw_val * SENSOR_G * (int64_t)range) / INT16_MAX;

	val->val1 = raw_val / 1000000LL;
	val->val2 = raw_val % 1000000LL;
}

static void channel_gyro_convert(struct sensor_value *val, int64_t raw_val, uint16_t range)
{
	/* 16 bit gyroscope. 2^15 bits represent the range in degrees/s */
	/* Converting from degrees/s to radians/s */

	val->val1 = ((raw_val * (int64_t)range * SENSOR_PI) / (180LL * INT16_MAX)) / 1000000LL;
	val->val2 = ((raw_val * (int64_t)range * SENSOR_PI) / (180LL * INT16_MAX)) % 1000000LL;
}

static uint8_t acc_odr_to_reg(const struct sensor_value *val)
{
	double odr = sensor_value_to_double((struct sensor_value *)val);
	uint8_t reg = 0;

	if ((odr >= 0.78125) && (odr < 1.5625)) {
		reg = BMI270_ACC_ODR_25D32_HZ;
	} else if ((odr >= 1.5625) && (odr < 3.125)) {
		reg = BMI270_ACC_ODR_25D16_HZ;
	} else if ((odr >= 3.125) && (odr < 6.25)) {
		reg = BMI270_ACC_ODR_25D8_HZ;
	} else if ((odr >= 6.25) && (odr < 12.5)) {
		reg = BMI270_ACC_ODR_25D4_HZ;
	} else if ((odr >= 12.5) && (odr < 25.0)) {
		reg = BMI270_ACC_ODR_25D2_HZ;
	} else if ((odr >= 25.0) && (odr < 50.0)) {
		reg = BMI270_ACC_ODR_25_HZ;
	} else if ((odr >= 50.0) && (odr < 100.0)) {
		reg = BMI270_ACC_ODR_50_HZ;
	} else if ((odr >= 100.0) && (odr < 200.0)) {
		reg = BMI270_ACC_ODR_100_HZ;
	} else if ((odr >= 200.0) && (odr < 400.0)) {
		reg = BMI270_ACC_ODR_200_HZ;
	} else if ((odr >= 400.0) && (odr < 800.0)) {
		reg = BMI270_ACC_ODR_400_HZ;
	} else if ((odr >= 800.0) && (odr < 1600.0)) {
		reg = BMI270_ACC_ODR_800_HZ;
	} else if (odr >= 1600.0) {
		reg = BMI270_ACC_ODR_1600_HZ;
	}
	return reg;
}

/* A rate of 0, or one the chip has not, turns the accelerometer off. */
static int set_accel_odr(const struct device *dev, const struct sensor_value *odr)
{
	struct bmi270_data *data = dev->data;

	data->acc_odr = acc_odr_to_reg(odr);
	return apply(dev);
}

static int set_accel_osr(const struct device *dev, const struct sensor_value *osr)
{
	struct bmi270_data *data = dev->data;
	uint8_t osr_bits;

	if (!IS_ENABLED(CONFIG_UNA_BMI270_LOW_POWER_MODE) &&
	    data->acc_odr >= BMI270_ACC_ODR_100_HZ) {
		/* Performance mode */
		/* osr->val2 should be unused */
		switch (osr->val1) {
		case 4:
			osr_bits = BMI270_ACC_BWP_OSR4_AVG1;
			break;
		case 2:
			osr_bits = BMI270_ACC_BWP_OSR2_AVG2;
			break;
		case 1:
			osr_bits = BMI270_ACC_BWP_NORM_AVG4;
			break;
		default:
			osr_bits = BMI270_ACC_BWP_CIC_AVG8;
			break;
		}
	} else {
		/* Power optimized mode */
		/* osr->val2 should be unused */
		switch (osr->val1) {
		case 1:
			osr_bits = BMI270_ACC_BWP_OSR4_AVG1;
			break;
		case 2:
			osr_bits = BMI270_ACC_BWP_OSR2_AVG2;
			break;
		case 4:
			osr_bits = BMI270_ACC_BWP_NORM_AVG4;
			break;
		case 8:
			osr_bits = BMI270_ACC_BWP_CIC_AVG8;
			break;
		case 16:
			osr_bits = BMI270_ACC_BWP_RES_AVG16;
			break;
		case 32:
			osr_bits = BMI270_ACC_BWP_RES_AVG32;
			break;
		case 64:
			osr_bits = BMI270_ACC_BWP_RES_AVG64;
			break;
		case 128:
			osr_bits = BMI270_ACC_BWP_RES_AVG128;
			break;
		default:
			return -ENOTSUP;
		}
	}

	data->acc_bwp = osr_bits;
	return apply(dev);
}

static int set_accel_range(const struct device *dev, const struct sensor_value *range)
{
	struct bmi270_data *data = dev->data;
	uint8_t reg;
	int ret;

	/* range->val2 is unused */
	switch (range->val1) {
	case 2:
		reg = BMI270_ACC_RANGE_2G;
		break;
	case 4:
		reg = BMI270_ACC_RANGE_4G;
		break;
	case 8:
		reg = BMI270_ACC_RANGE_8G;
		break;
	case 16:
		reg = BMI270_ACC_RANGE_16G;
		break;
	default:
		return -ENOTSUP;
	}

	ret = bmi270_reg_write(dev, BMI270_REG_ACC_RANGE, reg);
	if (ret == 0) {
		data->acc_range = range->val1;
	}

	return ret;
}

static uint8_t gyr_odr_to_reg(const struct sensor_value *val)
{
	double odr = sensor_value_to_double((struct sensor_value *)val);
	uint8_t reg = 0;

	if ((odr >= 25.0) && (odr < 50.0)) {
		reg = BMI270_GYR_ODR_25_HZ;
	} else if ((odr >= 50.0) && (odr < 100.0)) {
		reg = BMI270_GYR_ODR_50_HZ;
	} else if ((odr >= 100.0) && (odr < 200.0)) {
		reg = BMI270_GYR_ODR_100_HZ;
	} else if ((odr >= 200.0) && (odr < 400.0)) {
		reg = BMI270_GYR_ODR_200_HZ;
	} else if ((odr >= 400.0) && (odr < 800.0)) {
		reg = BMI270_GYR_ODR_400_HZ;
	} else if ((odr >= 800.0) && (odr < 1600.0)) {
		reg = BMI270_GYR_ODR_800_HZ;
	} else if ((odr >= 1600.0) && (odr < 3200.0)) {
		reg = BMI270_GYR_ODR_1600_HZ;
	} else if (odr >= 3200.0) {
		reg = BMI270_GYR_ODR_3200_HZ;
	}

	return reg;
}

/* A rate of 0, or one the chip has not, turns the gyroscope off. */
static int set_gyro_odr(const struct device *dev, const struct sensor_value *odr)
{
	struct bmi270_data *data = dev->data;

	data->gyr_odr = gyr_odr_to_reg(odr);
	return apply(dev);
}

static int set_gyro_osr(const struct device *dev, const struct sensor_value *osr)
{
	struct bmi270_data *data = dev->data;

	/* osr->val2 should be unused */
	switch (osr->val1) {
	case 4:
		data->gyr_bwp = BMI270_GYR_BWP_OSR4;
		break;
	case 2:
		data->gyr_bwp = BMI270_GYR_BWP_OSR2;
		break;
	default:
		data->gyr_bwp = BMI270_GYR_BWP_NORM;
		break;
	}
	return apply(dev);
}

static int set_gyro_range(const struct device *dev, const struct sensor_value *range)
{
	struct bmi270_data *data = dev->data;
	uint8_t reg;
	int ret;

	/* range->val2 is unused */
	switch (range->val1) {
	case 125:
		reg = BMI270_GYR_RANGE_125DPS;
		break;
	case 250:
		reg = BMI270_GYR_RANGE_250DPS;
		break;
	case 500:
		reg = BMI270_GYR_RANGE_500DPS;
		break;
	case 1000:
		reg = BMI270_GYR_RANGE_1000DPS;
		break;
	case 2000:
		reg = BMI270_GYR_RANGE_2000DPS;
		break;
	default:
		return -ENOTSUP;
	}

	ret = bmi270_reg_write(dev, BMI270_REG_GYR_RANGE, reg);
	if (ret == 0) {
		data->gyr_range = range->val1;
	}

	return ret;
}

static int write_config_file(const struct device *dev)
{
	const struct bmi270_config *cfg = dev->config;
	int ret = 0;

	for (size_t index = 0; index < sizeof(bmi270_config_file_base) && ret == 0;
	     index += BMI270_CONFIG_FILE_CHUNK) {
		size_t len = MIN(BMI270_CONFIG_FILE_CHUNK, sizeof(bmi270_config_file_base) - index);
		/* Where it goes, in words: four bits, then the eight above them. */
		uint8_t addr[2] = {(index / 2) & 0x0F, (index / 2) >> 4};

		ret = i2c_burst_write_dt(&cfg->i2c, BMI270_REG_INIT_ADDR_0, addr, sizeof(addr));
		if (ret == 0) {
			ret = i2c_burst_write_dt(&cfg->i2c, BMI270_REG_INIT_DATA,
						 &bmi270_config_file_base[index], len);
		}
	}

	return ret;
}

/* The chip's axes as the device has them. */
static void remap(const struct device *dev, const uint8_t *raw, int16_t *out)
{
	const struct bmi270_config *cfg = dev->config;

	for (int i = 0; i < 3; i++) {
		int32_t v = (int16_t)sys_get_le16(&raw[2 * cfg->axis_map[i]]);

		out[i] = cfg->axis_invert[i] ? CLAMP(-v, INT16_MIN, INT16_MAX) : v;
	}
}

/* The step count and the activity, which lie together. */
static int fetch_steps(const struct device *dev, bool count)
{
	struct bmi270_data *data = dev->data;
	uint8_t buf[3];
	uint16_t raw;
	int ret = bmi270_reg_read(dev, BMI270_REG_SC_OUT_0, buf, sizeof(buf));

	if (ret != 0) {
		return ret;
	}
	raw = sys_get_le16(buf);
	if (count) {
		data->steps += (uint16_t)(raw - data->steps_raw);
	}
	data->steps_raw = raw;
	data->activity = (buf[2] >> BMI270_WR_GEST_ACT_ACT_POS) & BMI270_WR_GEST_ACT_ACT_MSK;
	return 0;
}

static int bmi270_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	struct bmi270_data *data = dev->data;
	bool all = chan == SENSOR_CHAN_ALL;
	bool acc = all || chan == SENSOR_CHAN_ACCEL_XYZ;
	bool gyr = all || chan == SENSOR_CHAN_GYRO_XYZ;
	bool temp = all || chan == SENSOR_CHAN_DIE_TEMP;
	bool steps = all || chan == (enum sensor_channel)BMI270_CHAN_STEPS ||
		     chan == (enum sensor_channel)BMI270_CHAN_ACTIVITY;
	uint8_t buf[6];
	int ret = 0;

	if (!acc && !gyr && !temp && !steps) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	if (acc) {
		ret = bmi270_reg_read(dev, BMI270_REG_ACC_X_LSB, buf, sizeof(buf));
		if (ret == 0) {
			remap(dev, buf, data->acc);
		}
	}
	if (ret == 0 && gyr) {
		ret = bmi270_reg_read(dev, BMI270_REG_GYR_X_LSB, buf, sizeof(buf));
		if (ret == 0) {
			remap(dev, buf, data->gyr);
		}
	}
	if (ret == 0 && temp) {
		ret = bmi270_reg_read(dev, BMI270_REG_TEMPERATURE_0, buf, 2);
		data->temp = ret == 0 ? (int16_t)sys_get_le16(buf) : (int16_t)BMI270_TEMP_INVALID;
	}
	if (ret == 0 && steps) {
		ret = fetch_steps(dev, true);
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

static int bmi270_channel_get(const struct device *dev, enum sensor_channel chan,
			      struct sensor_value *val)
{
	struct bmi270_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (chan >= SENSOR_CHAN_ACCEL_X && chan <= SENSOR_CHAN_ACCEL_Z) {
		channel_accel_convert(val, data->acc[chan - SENSOR_CHAN_ACCEL_X], data->acc_range);
	} else if (chan == SENSOR_CHAN_ACCEL_XYZ) {
		for (int i = 0; i < 3; i++) {
			channel_accel_convert(&val[i], data->acc[i], data->acc_range);
		}
	} else if (chan >= SENSOR_CHAN_GYRO_X && chan <= SENSOR_CHAN_GYRO_Z) {
		channel_gyro_convert(val, data->gyr[chan - SENSOR_CHAN_GYRO_X], data->gyr_range);
	} else if (chan == SENSOR_CHAN_GYRO_XYZ) {
		for (int i = 0; i < 3; i++) {
			channel_gyro_convert(&val[i], data->gyr[i], data->gyr_range);
		}
	} else if (chan == SENSOR_CHAN_DIE_TEMP) {
		/* Invalid is also what the chip answers while the sensor is off. */
		if (data->temp == (int16_t)BMI270_TEMP_INVALID) {
			ret = -ENODATA;
		} else {
			int64_t micro_c =
				(((int64_t)data->temp * 1000000LL) / BMI270_TEMP_LSB_PER_DEG) +
				BMI270_TEMP_CENTER_OFFSET;

			val->val1 = micro_c / 1000000LL;
			val->val2 = micro_c % 1000000LL;
		}
	} else if (chan == (enum sensor_channel)BMI270_CHAN_STEPS) {
		val->val1 = data->steps;
		val->val2 = 0;
	} else if (chan == (enum sensor_channel)BMI270_CHAN_ACTIVITY) {
		val->val1 = data->activity;
		val->val2 = 0;
	} else {
		ret = -ENOTSUP;
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

/* What turns each feature on: a bit of a feature register. */
static const struct {
	uint8_t page, reg;
	uint16_t bit;
} feature_enables[] = {
	{BMI270_FEAT_PAGE_SC_26, BMI270_FEAT_SC_26, BMI270_SC_26_EN_COUNTER},
	{BMI270_FEAT_PAGE_SC_26, BMI270_FEAT_SC_26, BMI270_SC_26_EN_DETECTOR},
	{BMI270_FEAT_PAGE_SC_26, BMI270_FEAT_SC_26, BMI270_SC_26_EN_ACTIVITY},
	{BMI270_FEAT_PAGE_WR_WAKEUP, BMI270_FEAT_WR_WAKEUP_1, BMI270_WR_WAKEUP_1_ENABLE},
	{BMI270_FEAT_PAGE_ANYMO, BMI270_FEAT_ANYMO_2, BMI270_ANYMO_2_ENABLE},
	{BMI270_FEAT_PAGE_NOMO, BMI270_FEAT_NOMO_2, BMI270_ANYMO_2_ENABLE},
};

int bmi270_feature_set(const struct device *dev, uint8_t feature, bool on)
{
	struct bmi270_data *data = dev->data;
	uint8_t features = on ? data->features | feature : data->features & ~feature;
	int index = find_lsb_set(feature) - 1;
	int ret, ret_end;

	/* The accelerometer first, and last to go. */
	if (on) {
		data->features = features;
		ret = apply(dev);
		if (ret != 0) {
			goto failed;
		}
	}
	ret = bmi270_feat_begin(dev);
	if (ret == 0 && on && feature == BMI270_FEATURE_ANY_MOTION) {
		ret = bmi270_feat_update(dev, BMI270_FEAT_PAGE_ANYMO, BMI270_FEAT_ANYMO_1,
					 BMI270_ANYMO_1_DURATION_MSK | BMI270_ANYMO_1_SELECT_XYZ,
					 data->anymo_1);
		if (ret == 0) {
			ret = bmi270_feat_update(dev, BMI270_FEAT_PAGE_ANYMO, BMI270_FEAT_ANYMO_2,
						 BMI270_ANYMO_2_THRESHOLD_MSK, data->anymo_2);
		}
	}
	if (ret == 0) {
		ret = bmi270_feat_update(dev, feature_enables[index].page,
					 feature_enables[index].reg, feature_enables[index].bit,
					 on ? feature_enables[index].bit : 0);
	}
	ret_end = bmi270_feat_end(dev);
	ret = ret != 0 ? ret : ret_end;
	if (ret != 0) {
		goto failed;
	}
	if (on && feature == BMI270_FEATURE_STEPS) {
		/* Counting goes on from what the chip has now. */
		ret = fetch_steps(dev, false);
	}
	if (!on) {
		data->features = features;
		ret = apply(dev);
	}
	return ret;

failed:
	LOG_ERR("feature 0x%02x %s: %d", feature, on ? "on" : "off", ret);
	data->features &= ~feature;
	(void)apply(dev);
	return ret;
}

static int step_attr_set(const struct device *dev, enum sensor_attribute attr,
			 const struct sensor_value *val)
{
	struct bmi270_data *data = dev->data;
	int ret, ret_end;

	switch ((int)attr) {
	case BMI270_ATTR_ENABLE:
		return bmi270_feature_set(dev, BMI270_FEATURE_STEPS, val->val1 != 0);
	case BMI270_ATTR_STEP_DETECTOR:
		return bmi270_feature_set(dev, BMI270_FEATURE_STEP_DETECTOR, val->val1 != 0);
	case BMI270_ATTR_STEP_WATERMARK:
		if (val->val1 < 0 ||
		    val->val1 > BMI270_SC_26_WATERMARK_MSK * BMI270_SC_26_WATERMARK_STEPS) {
			return -EINVAL;
		}
		ret = bmi270_feat_begin(dev);
		if (ret == 0) {
			ret = bmi270_feat_update(dev, BMI270_FEAT_PAGE_SC_26, BMI270_FEAT_SC_26,
						 BMI270_SC_26_WATERMARK_MSK,
						 val->val1 / BMI270_SC_26_WATERMARK_STEPS);
		}
		ret_end = bmi270_feat_end(dev);
		return ret != 0 ? ret : ret_end;
	case BMI270_ATTR_STEP_RESET:
		/* The chip resets its count only while it counts. */
		if ((data->features & BMI270_FEATURE_STEPS) != 0) {
			ret = bmi270_feat_begin(dev);
			if (ret == 0) {
				ret = bmi270_feat_update(dev, BMI270_FEAT_PAGE_SC_26,
							 BMI270_FEAT_SC_26,
							 BMI270_SC_26_RESET_COUNTER,
							 BMI270_SC_26_RESET_COUNTER);
			}
			ret_end = bmi270_feat_end(dev);
			ret = ret != 0 ? ret : ret_end;
			if (ret != 0) {
				return ret;
			}
			/* Its features run at 50 Hz: a few rounds for it to be done. */
			k_msleep(60);
			ret = fetch_steps(dev, false);
			if (ret != 0) {
				return ret;
			}
		}
		data->steps = 0;
		return 0;
	default:
		return -ENOTSUP;
	}
}

/* SENSOR_ATTR_SLOPE_TH in m/s^2; the register has 1 g as 2048. */
static int set_anymo_threshold(const struct device *dev, const struct sensor_value *val)
{
	struct bmi270_data *data = dev->data;
	int64_t lsbs = sensor_value_to_micro(val) * (BMI270_ANYMO_2_THRESHOLD_MSK + 1) / SENSOR_G;

	if (lsbs <= 0) {
		LOG_ERR("Threshold too low!");
		return -EINVAL;
	}
	data->anymo_2 = MIN(lsbs, BMI270_ANYMO_2_THRESHOLD_MSK);
	return 0;
}

/* SENSOR_ATTR_SLOPE_DUR in ms; the register counts samples of 20 ms. */
static int set_anymo_duration(const struct device *dev, const struct sensor_value *val)
{
	struct bmi270_data *data = dev->data;

	data->anymo_1 = CLAMP(val->val1 / 20, 0, BMI270_ANYMO_1_DURATION_MSK) |
			BMI270_ANYMO_1_SELECT_XYZ;
	return 0;
}

static int bmi270_attr_set(const struct device *dev, enum sensor_channel chan,
			   enum sensor_attribute attr, const struct sensor_value *val)
{
	struct bmi270_data *data = dev->data;
	int ret = -ENOTSUP;

	k_mutex_lock(&data->lock, K_FOREVER);
	if ((chan == SENSOR_CHAN_ACCEL_X) || (chan == SENSOR_CHAN_ACCEL_Y) ||
	    (chan == SENSOR_CHAN_ACCEL_Z) || (chan == SENSOR_CHAN_ACCEL_XYZ)) {
		switch (attr) {
		case SENSOR_ATTR_SAMPLING_FREQUENCY:
			ret = set_accel_odr(dev, val);
			break;
		case SENSOR_ATTR_OVERSAMPLING:
			ret = set_accel_osr(dev, val);
			break;
		case SENSOR_ATTR_FULL_SCALE:
			ret = set_accel_range(dev, val);
			break;
		/* These two are taken when SENSOR_TRIG_MOTION is set. */
		case SENSOR_ATTR_SLOPE_DUR:
			ret = set_anymo_duration(dev, val);
			break;
		case SENSOR_ATTR_SLOPE_TH:
			ret = set_anymo_threshold(dev, val);
			break;
		default:
			ret = -ENOTSUP;
		}
	} else if ((chan == SENSOR_CHAN_GYRO_X) || (chan == SENSOR_CHAN_GYRO_Y) ||
		   (chan == SENSOR_CHAN_GYRO_Z) || (chan == SENSOR_CHAN_GYRO_XYZ)) {
		switch (attr) {
		case SENSOR_ATTR_SAMPLING_FREQUENCY:
			ret = set_gyro_odr(dev, val);
			break;
		case SENSOR_ATTR_OVERSAMPLING:
			ret = set_gyro_osr(dev, val);
			break;
		case SENSOR_ATTR_FULL_SCALE:
			ret = set_gyro_range(dev, val);
			break;
		default:
			ret = -ENOTSUP;
		}
	} else if (chan == (enum sensor_channel)BMI270_CHAN_STEPS) {
		ret = step_attr_set(dev, attr, val);
	} else if (chan == (enum sensor_channel)BMI270_CHAN_ACTIVITY &&
		   attr == (enum sensor_attribute)BMI270_ATTR_ENABLE) {
		ret = bmi270_feature_set(dev, BMI270_FEATURE_ACTIVITY, val->val1 != 0);
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

/* The mounting and the wrist's angles, into the feature registers. */
static int write_features(const struct device *dev)
{
	const struct bmi270_config *cfg = dev->config;
	uint16_t axes = 0;
	int ret;

	for (int i = 0; i < 3; i++) {
		axes |= (cfg->axis_map[i] | cfg->axis_invert[i] << 2) << (3 * i);
	}
	ret = bmi270_feat_update(dev, BMI270_FEAT_PAGE_GEN_SET_1, BMI270_FEAT_GEN_SET_1,
				 BMI270_GEN_SET_1_AXES_MSK, axes);
	for (int i = 0; i < BMI270_WR_WAKEUP_PARAMS && ret == 0 && cfg->wrist_wake_given; i++) {
		ret = bmi270_feat_write(dev, BMI270_FEAT_PAGE_WR_WAKEUP,
					BMI270_FEAT_WR_WAKEUP_2 + 2 * i, cfg->wrist_wake[i]);
	}
	return ret;
}

static int bmi270_init(const struct device *dev)
{
	const struct bmi270_config *cfg = dev->config;
	struct bmi270_data *data = dev->data;
	uint8_t chip_id;
	uint8_t msg;
	uint8_t tries;
	int ret;

	if (!device_is_ready(cfg->i2c.bus)) {
		LOG_ERR("Bus not ready");
		return -ENODEV;
	}

	data->dev = dev;
	k_mutex_init(&data->lock);

	/* As a reset leaves the chip, but for the rates: both sensors are off. */
	data->acc_range = 8;
	data->acc_bwp = BMI270_ACC_BWP_NORM_AVG4;
	data->gyr_range = 2000;
	data->gyr_bwp = BMI270_GYR_BWP_NORM;
	data->anymo_1 = 5 | BMI270_ANYMO_1_SELECT_XYZ;
	data->anymo_2 = 0xAA;
	data->temp = (int16_t)BMI270_TEMP_INVALID;

	k_usleep(BMI270_POWER_ON_TIME);

	ret = bmi270_reg_read(dev, BMI270_REG_CHIP_ID, &chip_id, 1);
	if (ret != 0) {
		LOG_ERR("CHIP_ID read failed: %d", ret);
		return ret;
	}

	if (chip_id != BMI270_CHIP_ID) {
		LOG_ERR("Unexpected chip id (0x%02x). Expected (0x%02x)", chip_id, BMI270_CHIP_ID);
		return -EIO;
	}

	ret = bmi270_reg_write(dev, BMI270_REG_CMD, BMI270_CMD_SOFT_RESET);
	if (ret != 0) {
		LOG_ERR("Soft reset write failed: %d", ret);
		return ret;
	}

	k_usleep(BMI270_SOFT_RESET_TIME);

	/* Advanced power save off: until bmi270_feat_end() below. */
	ret = bmi270_feat_begin(dev);
	if (ret != 0) {
		return ret;
	}

	ret = bmi270_reg_write(dev, BMI270_REG_INIT_CTRL, BMI270_PREPARE_CONFIG_LOAD);
	if (ret != 0) {
		LOG_ERR("INIT_CTRL prepare failed: %d", ret);
		return ret;
	}

	ret = write_config_file(dev);
	if (ret != 0) {
		LOG_ERR("Config file write failed: %d", ret);
		return ret;
	}

	ret = bmi270_reg_write(dev, BMI270_REG_INIT_CTRL, BMI270_COMPLETE_CONFIG_LOAD);
	if (ret != 0) {
		LOG_ERR("INIT_CTRL complete failed: %d", ret);
		return ret;
	}

	/* Timeout after BMI270_CONFIG_FILE_RETRIES x
	 * BMI270_CONFIG_FILE_POLL_PERIOD_US microseconds.
	 * If tries is BMI270_CONFIG_FILE_RETRIES by the end of the loop,
	 * report an error
	 */
	for (tries = 0; tries <= BMI270_CONFIG_FILE_RETRIES; tries++) {
		ret = bmi270_reg_read(dev, BMI270_REG_INTERNAL_STATUS, &msg, 1);
		if (ret != 0) {
			LOG_ERR("INTERNAL_STATUS read failed: %d", ret);
			return ret;
		}

		msg &= BMI270_INST_MESSAGE_MSK;
		if (msg == BMI270_INST_MESSAGE_INIT_OK) {
			break;
		}

		k_usleep(BMI270_CONFIG_FILE_POLL_PERIOD_US);
	}

	if (tries > BMI270_CONFIG_FILE_RETRIES) {
		LOG_ERR("Config load timeout (INTERNAL_STATUS not INIT_OK)");
		return -EIO;
	}

	ret = write_features(dev);
	if (ret != 0) {
		LOG_ERR("Feature setup failed: %d", ret);
		return ret;
	}

	ret = bmi270_init_interrupts(dev);
	if (ret != 0) {
		LOG_ERR("bmi270_init_interrupts returned %d", ret);
		return ret;
	}

	/* Nothing is on: with this the chip is suspended. */
	return bmi270_feat_end(dev);
}

static DEVICE_API(sensor, bmi270_driver_api) = {
	.sample_fetch = bmi270_sample_fetch,
	.channel_get = bmi270_channel_get,
	.attr_set = bmi270_attr_set,
	.trigger_set = bmi270_trigger_set,
};

#define BMI270_AXIS_OK(inst, i)                                                                    \
	BUILD_ASSERT(DT_INST_PROP_BY_IDX(inst, axes, i) < 3 &&                                 \
		     DT_INST_PROP_BY_IDX(inst, axes_inverted, i) < 2,                                \
		     "axes is of 0 to 2, axes-inverted of 0 and 1")

#define BMI270_CREATE_INST(inst)                                                                   \
	BUILD_ASSERT(DT_INST_PROP_LEN(inst, axes) == 3 &&                                      \
		     DT_INST_PROP_LEN(inst, axes_inverted) == 3,                                     \
		     "axes and axes-inverted have an entry for each of x, y and z");             \
	BMI270_AXIS_OK(inst, 0);                                                                   \
	BMI270_AXIS_OK(inst, 1);                                                                   \
	BMI270_AXIS_OK(inst, 2);                                                                   \
	BUILD_ASSERT((BIT(DT_INST_PROP_BY_IDX(inst, axes, 0)) |                                \
		      BIT(DT_INST_PROP_BY_IDX(inst, axes, 1)) |                                \
		      BIT(DT_INST_PROP_BY_IDX(inst, axes, 2))) == 7,                           \
		     "axes names each axis of the chip once");                                 \
	BUILD_ASSERT(DT_INST_PROP_LEN_OR(inst, wrist_wake_params, BMI270_WR_WAKEUP_PARAMS) ==      \
			     BMI270_WR_WAKEUP_PARAMS,                                              \
		     "wrist-wake-params has six entries");                                         \
                                                                                                   \
	static struct bmi270_data bmi270_drv_##inst;                                               \
                                                                                                   \
	static const struct bmi270_config bmi270_config_##inst = {                                 \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.irq = GPIO_DT_SPEC_INST_GET_OR(inst, irq_gpios, {0}),                             \
		.int_pin = DT_INST_PROP(inst, int_pin),                                            \
		.axis_map = DT_INST_PROP(inst, axes),                                          \
		.axis_invert = DT_INST_PROP(inst, axes_inverted),                                    \
		.wrist_wake = DT_INST_PROP_OR(inst, wrist_wake_params, {0}),                       \
		.wrist_wake_given = DT_INST_NODE_HAS_PROP(inst, wrist_wake_params),                \
	};                                                                                         \
                                                                                                   \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, bmi270_init, NULL, &bmi270_drv_##inst,                  \
				     &bmi270_config_##inst, POST_KERNEL,                           \
				     CONFIG_SENSOR_INIT_PRIORITY, &bmi270_driver_api);

DT_INST_FOREACH_STATUS_OKAY(BMI270_CREATE_INST);
