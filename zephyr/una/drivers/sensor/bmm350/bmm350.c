/*
 * Copyright (c) 2024 Bosch Sensortec GmbH
 * Copyright (c) 2025 Croxel, Inc.
 * Copyright (c) 2025 CogniPilot Foundation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Bosch BMM350 on I2C, for a watch.
 *
 * Zephyr's driver (drivers/sensor/bosch/bmm350), cut down to what is used
 * and changed where it did not suit:
 *
 * - Plain I2C. There, the driver is built on RTIO and selects I2C_RTIO,
 *   which changes the controller's driver under every other part on every
 *   bus. With it went I3C, streaming and the data-ready trigger: the field
 *   is read when asked for.
 * - The chip is suspended, at a few microamps, when the driver has started,
 *   and measures from when a sampling frequency is set until that is set
 *   to 0. There it measures from the start, and stops only through device
 *   power management.
 * - The field is worked out as Bosch's own code does, in floating point,
 *   and kept in nanotesla. There it is in whole microtesla, of which the
 *   earth's field has some tens: too coarse to tell a direction by. The
 *   trim of the y axis' sensitivity and the z axis' drift is as in Bosch's.
 * - The mounting is in the devicetree (axes): the field comes out in the
 *   device's frame.
 * - A chip that does not answer for its OTP is given up after a while.
 */

#define DT_DRV_COMPAT una_bmm350

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "bmm350.h"

LOG_MODULE_REGISTER(una_bmm350, CONFIG_SENSOR_LOG_LEVEL);

/* The chip's trim, from its OTP. Temperatures in degrees, offsets in uT. */
struct bmm350_trim {
	float offset[3], t_offs;
	float sens[3], t_sens;
	float tco[3];
	float tcs[3];
	float t0;
	float cross_x_y, cross_y_x, cross_z_x, cross_z_y;
};

struct bmm350_data {
	struct k_mutex lock;
	struct bmm350_trim trim;
	/* The device's axes, nT; hundredths of a degree. */
	int32_t mag[3];
	int32_t temperature;
	/* As asked for; a rate of 0 is suspended. */
	uint8_t odr, avg;
};

struct bmm350_config {
	struct i2c_dt_spec i2c;
	uint8_t axis_map[3];
	uint8_t axis_invert[3];
};

static int bmm350_reg_read(const struct device *dev, uint8_t reg, uint8_t *buf, size_t size)
{
	const struct bmm350_config *cfg = dev->config;
	uint8_t raw[BMM350_DUMMY_BYTES + 12];
	int ret;

	__ASSERT_NO_MSG(size <= sizeof(raw) - BMM350_DUMMY_BYTES);

	ret = i2c_burst_read_dt(&cfg->i2c, reg, raw, BMM350_DUMMY_BYTES + size);
	memcpy(buf, &raw[BMM350_DUMMY_BYTES], size);
	return ret;
}

static int bmm350_reg_write(const struct device *dev, uint8_t reg, uint8_t val)
{
	const struct bmm350_config *cfg = dev->config;

	return i2c_reg_write_byte_dt(&cfg->i2c, reg, val);
}

static int bmm350_read_otp_word(const struct device *dev, uint8_t addr, uint16_t *lsb_msb)
{
	uint8_t status = 0, msb, lsb;
	int ret;

	/* Set OTP command at specified address */
	ret = bmm350_reg_write(dev, BMM350_REG_OTP_CMD_REG,
			       BMM350_OTP_CMD_DIR_READ | (addr & BMM350_OTP_WORD_ADDR_MSK));
	if (ret != 0) {
		return ret;
	}

	for (int tries = 0; (status & BMM350_OTP_STATUS_CMD_DONE) == 0; tries++) {
		if (tries == BMM350_OTP_TRIES) {
			return -ETIMEDOUT;
		}
		ret = bmm350_reg_read(dev, BMM350_REG_OTP_STATUS_REG, &status, 1);
		if (ret != 0) {
			return ret;
		}
		if ((status & BMM350_OTP_STATUS_ERROR_MSK) != 0) {
			LOG_ERR("OTP error code: 0x%02x", status & BMM350_OTP_STATUS_ERROR_MSK);
			return -EIO;
		}
	}

	ret = bmm350_reg_read(dev, BMM350_REG_OTP_DATA_MSB_REG, &msb, 1);
	if (ret == 0) {
		ret = bmm350_reg_read(dev, BMM350_REG_OTP_DATA_LSB_REG, &lsb, 1);
	}
	*lsb_msb = (uint16_t)(msb << 8) | lsb;

	return ret;
}

/* A field of a word of the OTP, as a signed number. */
static float signed_bits(uint16_t value, int bits)
{
	return (float)sign_extend(value, bits - 1);
}

#define OTP_LSB(word) ((word) & 0xFF)
#define OTP_MSB(word) ((word) >> 8)

static void bmm350_update_mag_off_sens(struct bmm350_data *data, const uint16_t *otp)
{
	struct bmm350_trim *trim = &data->trim;
	uint16_t off_x = otp[BMM350_MAG_OFFSET_X] & 0x0FFF;
	uint16_t off_y = ((otp[BMM350_MAG_OFFSET_X] & 0xF000) >> 4) +
			 OTP_LSB(otp[BMM350_MAG_OFFSET_Y]);
	uint16_t off_z = (otp[BMM350_MAG_OFFSET_Y] & 0x0F00) + OTP_LSB(otp[BMM350_MAG_OFFSET_Z]);

	trim->offset[0] = signed_bits(off_x, 12);
	trim->offset[1] = signed_bits(off_y, 12);
	trim->offset[2] = signed_bits(off_z, 12);
	trim->t_offs = signed_bits(OTP_LSB(otp[BMM350_TEMP_OFF_SENS]), 8) / 5.0f;

	trim->sens[0] = signed_bits(OTP_MSB(otp[BMM350_MAG_SENS_X]), 8) / 256.0f;
	trim->sens[1] = signed_bits(OTP_LSB(otp[BMM350_MAG_SENS_Y]), 8) / 256.0f + 0.01f;
	trim->sens[2] = signed_bits(OTP_MSB(otp[BMM350_MAG_SENS_Z]), 8) / 256.0f;
	trim->t_sens = signed_bits(OTP_MSB(otp[BMM350_TEMP_OFF_SENS]), 8) / 512.0f;

	trim->tco[0] = signed_bits(OTP_LSB(otp[BMM350_MAG_TCO_X]), 8) / 32.0f;
	trim->tco[1] = signed_bits(OTP_LSB(otp[BMM350_MAG_TCO_Y]), 8) / 32.0f;
	trim->tco[2] = signed_bits(OTP_LSB(otp[BMM350_MAG_TCO_Z]), 8) / 32.0f;

	trim->tcs[0] = signed_bits(OTP_MSB(otp[BMM350_MAG_TCS_X]), 8) / 16384.0f;
	trim->tcs[1] = signed_bits(OTP_MSB(otp[BMM350_MAG_TCS_Y]), 8) / 16384.0f;
	trim->tcs[2] = signed_bits(OTP_MSB(otp[BMM350_MAG_TCS_Z]), 8) / 16384.0f - 0.0001f;

	trim->t0 = signed_bits(otp[BMM350_MAG_DUT_T_0], 16) / 512.0f + 23.0f;

	trim->cross_x_y = signed_bits(OTP_LSB(otp[BMM350_CROSS_X_Y]), 8) / 800.0f;
	trim->cross_y_x = signed_bits(OTP_MSB(otp[BMM350_CROSS_Y_X]), 8) / 800.0f;
	trim->cross_z_x = signed_bits(OTP_LSB(otp[BMM350_CROSS_Z_X]), 8) / 800.0f;
	trim->cross_z_y = signed_bits(OTP_MSB(otp[BMM350_CROSS_Z_Y]), 8) / 800.0f;
}

static int bmm350_otp_dump_after_boot(const struct device *dev)
{
	struct bmm350_data *data = dev->data;
	uint16_t otp[BMM350_OTP_DATA_LENGTH];

	for (uint8_t idx = 0; idx < BMM350_OTP_DATA_LENGTH; idx++) {
		int ret = bmm350_read_otp_word(dev, idx, &otp[idx]);

		if (ret != 0) {
			LOG_ERR("OTP word %u: %d", idx, ret);
			return ret;
		}
	}
	bmm350_update_mag_off_sens(data, otp);

	return 0;
}

/* The command the chip last carried out. */
static int bmm350_pmu_cmd_done(const struct device *dev, uint8_t *cmd)
{
	uint8_t status;
	int ret = bmm350_reg_read(dev, BMM350_REG_PMU_CMD_STATUS_0, &status, 1);

	*cmd = status >> BMM350_PMU_CMD_VALUE_POS;
	return ret;
}

/*
 * The magnetic reset, a bit reset and a flux guide reset: at the start, and
 * what the sensor needs after a field of 400 mT. In suspend only.
 */
static int bmm350_magnetic_reset(const struct device *dev)
{
	uint8_t done;
	int ret;

	ret = bmm350_reg_write(dev, BMM350_REG_PMU_CMD, BMM350_PMU_CMD_BR);
	if (ret != 0) {
		return ret;
	}
	k_usleep(BMM350_BR_DELAY);
	ret = bmm350_pmu_cmd_done(dev, &done);
	if (ret != 0 || done != BMM350_PMU_CMD_STATUS_0_BR) {
		return ret != 0 ? ret : -EIO;
	}

	ret = bmm350_reg_write(dev, BMM350_REG_PMU_CMD, BMM350_PMU_CMD_FGR);
	if (ret != 0) {
		return ret;
	}
	k_usleep(BMM350_FGR_DELAY);
	ret = bmm350_pmu_cmd_done(dev, &done);
	if (ret != 0 || done != BMM350_PMU_CMD_STATUS_0_FGR) {
		return ret != 0 ? ret : -EIO;
	}

	return bmm350_reg_write(dev, BMM350_REG_PMU_CMD, BMM350_PMU_CMD_SUS);
}

static int bmm350_suspend(const struct device *dev)
{
	int ret = bmm350_reg_write(dev, BMM350_REG_PMU_CMD, BMM350_PMU_CMD_SUS);

	k_usleep(BMM350_GOTO_SUSPEND_DELAY);
	return ret;
}

/* The rate and the averaging as asked for, and measuring; or suspended. */
static int apply(const struct device *dev)
{
	struct bmm350_data *data = dev->data;
	uint8_t avg = data->avg;
	int ret;

	/* The rate can be changed in suspend only. */
	ret = bmm350_suspend(dev);
	if (ret != 0 || data->odr == 0) {
		return ret;
	}

	/* Reduce the averaging when too much for the chosen rate */
	if (data->odr == BMM350_ODR_400HZ) {
		avg = BMM350_AVG_1;
	} else if (data->odr == BMM350_ODR_200HZ) {
		avg = MIN(avg, BMM350_AVG_2);
	} else if (data->odr == BMM350_ODR_100HZ) {
		avg = MIN(avg, BMM350_AVG_4);
	}

	ret = bmm350_reg_write(dev, BMM350_REG_PMU_CMD_AGGR_SET,
			       data->odr | avg << BMM350_AVG_POS);
	if (ret == 0) {
		ret = bmm350_reg_write(dev, BMM350_REG_PMU_CMD, BMM350_PMU_CMD_UPD_OAE);
	}
	if (ret != 0) {
		return ret;
	}
	k_usleep(BMM350_UPD_OAE_DELAY);

	ret = bmm350_reg_write(dev, BMM350_REG_PMU_CMD, BMM350_PMU_CMD_NM);
	k_usleep(BMM350_SUSPEND_TO_NORMAL_DELAY);
	return ret;
}

static int32_t raw24(const uint8_t *buf)
{
	return sign_extend(sys_get_le24(buf), 23);
}

/* Bosch's arithmetic (bmm350_get_compensated_mag_xyz_temp_data), in uT and degrees. */
static void compensate(const struct bmm350_trim *trim, const uint8_t *raw, float *mag, float *temp)
{
	/* What a count is worth: the gains of the chip's stages multiplied out. */
	const float count = 1000000.0f / 1048576.0f;
	const float gain = (1.0f / 1.5f) * 0.714607238769531f;
	const float xy = count / (14.55f * 19.46f * gain);
	const float z = count / (9.0f * 31.0f * gain);
	const float degrees = 1.0f / (0.00204f * gain * 1048576.0f);
	float out[3] = {raw24(&raw[0]) * xy, raw24(&raw[3]) * xy, raw24(&raw[6]) * z};
	float t = raw24(&raw[9]) * degrees;
	float cross;

	if (t > 0.0f) {
		t -= 25.49f;
	} else if (t < 0.0f) {
		t += 25.49f;
	}
	t = (1.0f + trim->t_sens) * t + trim->t_offs;

	for (int i = 0; i < 3; i++) {
		out[i] *= 1.0f + trim->sens[i];
		out[i] += trim->offset[i];
		out[i] += trim->tco[i] * (t - trim->t0);
		out[i] /= 1.0f + trim->tcs[i] * (t - trim->t0);
	}

	cross = 1.0f - trim->cross_y_x * trim->cross_x_y;
	mag[0] = (out[0] - trim->cross_x_y * out[1]) / cross;
	mag[1] = (out[1] - trim->cross_y_x * out[0]) / cross;
	mag[2] = out[2] + (out[0] * (trim->cross_y_x * trim->cross_z_y - trim->cross_z_x) -
			   out[1] * (trim->cross_z_y - trim->cross_x_y * trim->cross_z_x)) /
				  cross;
	*temp = t;
}

static int bmm350_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct bmm350_config *cfg = dev->config;
	struct bmm350_data *data = dev->data;
	uint8_t raw[12];
	float mag[3], temp;
	int ret;

	ARG_UNUSED(chan);

	k_mutex_lock(&data->lock, K_FOREVER);
	if (data->odr == 0) {
		/* Suspended, the chip has the last it measured, however old. */
		ret = -ENODATA;
	} else {
		ret = bmm350_reg_read(dev, BMM350_REG_MAG_X_XLSB, raw, sizeof(raw));
	}
	if (ret == 0) {
		compensate(&data->trim, raw, mag, &temp);
		for (int i = 0; i < 3; i++) {
			float ut = mag[cfg->axis_map[i]];

			data->mag[i] = (int32_t)((cfg->axis_invert[i] ? -ut : ut) * 1000.0f);
		}
		data->temperature = (int32_t)(temp * 100.0f);
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

/* nT to Gauss, of which one is 100 uT. */
static void bmm350_convert(struct sensor_value *val, int32_t nt)
{
	val->val1 = nt / 100000;
	val->val2 = (nt % 100000) * 10;
}

static int bmm350_channel_get(const struct device *dev, enum sensor_channel chan,
			      struct sensor_value *val)
{
	struct bmm350_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);
	switch (chan) {
	case SENSOR_CHAN_MAGN_X:
	case SENSOR_CHAN_MAGN_Y:
	case SENSOR_CHAN_MAGN_Z:
		bmm350_convert(val, data->mag[chan - SENSOR_CHAN_MAGN_X]);
		break;
	case SENSOR_CHAN_MAGN_XYZ:
		bmm350_convert(val, data->mag[0]);
		bmm350_convert(val + 1, data->mag[1]);
		bmm350_convert(val + 2, data->mag[2]);
		break;
	case SENSOR_CHAN_DIE_TEMP:
		val->val1 = data->temperature / 100;
		val->val2 = (data->temperature % 100) * 10000;
		break;
	default:
		ret = -ENOTSUP;
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

/* The rate at or above what is asked for; 0 is none. */
static uint8_t mag_odr_to_reg(const struct sensor_value *val)
{
	int64_t odr = sensor_value_to_micro(val);

	if (odr <= 0) {
		return 0;
	}
	for (uint8_t reg = BMM350_ODR_1_5625HZ; reg > BMM350_ODR_400HZ; reg--) {
		/* 1.5625 Hz, and twice that with each step. */
		if (odr <= 1562500LL << (BMM350_ODR_1_5625HZ - reg)) {
			return reg;
		}
	}
	return BMM350_ODR_400HZ;
}

static int bmm350_attr_set(const struct device *dev, enum sensor_channel chan,
			   enum sensor_attribute attr, const struct sensor_value *val)
{
	struct bmm350_data *data = dev->data;
	int ret = 0;

	ARG_UNUSED(chan);

	k_mutex_lock(&data->lock, K_FOREVER);
	switch (attr) {
	case SENSOR_ATTR_SAMPLING_FREQUENCY:
		data->odr = mag_odr_to_reg(val);
		ret = apply(dev);
		break;
	case SENSOR_ATTR_OVERSAMPLING:
		if (val->val1 != 1 && val->val1 != 2 && val->val1 != 4 && val->val1 != 8) {
			LOG_ERR("unsupported oversampling rate");
			ret = -EINVAL;
			break;
		}
		data->avg = find_lsb_set(val->val1) - 1;
		/* Taken with the next rate when it is not measuring. */
		if (data->odr != 0) {
			ret = apply(dev);
		}
		break;
	default:
		ret = -ENOTSUP;
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

static DEVICE_API(sensor, bmm350_api_funcs) = {
	.attr_set = bmm350_attr_set,
	.sample_fetch = bmm350_sample_fetch,
	.channel_get = bmm350_channel_get,
};

static int bmm350_init(const struct device *dev)
{
	const struct bmm350_config *cfg = dev->config;
	struct bmm350_data *data = dev->data;
	uint8_t chip_id, err;
	int ret;

	if (!device_is_ready(cfg->i2c.bus)) {
		LOG_ERR("Bus not ready");
		return -ENODEV;
	}

	k_mutex_init(&data->lock);
	/* As a reset leaves it. */
	data->avg = BMM350_AVG_2;

	ret = bmm350_reg_read(dev, BMM350_REG_CHIP_ID, &chip_id, 1);
	if (ret != 0) {
		LOG_ERR("failed reading chip id");
		return ret;
	}
	if (chip_id != BMM350_CHIP_ID) {
		LOG_ERR("invalid chip id 0x%x", chip_id);
		return -EIO;
	}

	ret = bmm350_reg_write(dev, BMM350_REG_CMD, BMM350_CMD_SOFTRESET);
	if (ret != 0) {
		LOG_ERR("failed to issue soft reset");
		return ret;
	}
	k_usleep(BMM350_SOFT_RESET_DELAY);

	ret = bmm350_otp_dump_after_boot(dev);
	if (ret != 0) {
		return ret;
	}

	ret = bmm350_reg_write(dev, BMM350_REG_OTP_CMD_REG, BMM350_OTP_CMD_PWR_OFF_OTP);
	if (ret != 0) {
		LOG_ERR("failed to power off the OTP");
		return ret;
	}

	/* Leaves it suspended. */
	ret = bmm350_magnetic_reset(dev);
	if (ret != 0) {
		LOG_ERR("failed to perform magnetic reset");
		(void)bmm350_suspend(dev);
		return ret;
	}

	/* Reading it clears it; a command error has been seen in it after the start. */
	ret = bmm350_reg_read(dev, BMM350_REG_ERR_REG, &err, 1);
	LOG_DBG("ERR_REG 0x%02x after start", err);

	return ret;
}

#define BMM350_AXIS_OK(inst, i)                                                                    \
	BUILD_ASSERT(DT_INST_PROP_BY_IDX(inst, axes, i) < 3 &&                                     \
		     DT_INST_PROP_BY_IDX(inst, axes_inverted, i) < 2,                              \
		     "axes is of 0 to 2, axes-inverted of 0 and 1")

#define BMM350_DEFINE(inst)                                                                        \
	BUILD_ASSERT(DT_INST_PROP_LEN(inst, axes) == 3 &&                                          \
		     DT_INST_PROP_LEN(inst, axes_inverted) == 3,                                   \
		     "axes and axes-inverted have an entry for each of x, y and z");               \
	BMM350_AXIS_OK(inst, 0);                                                                   \
	BMM350_AXIS_OK(inst, 1);                                                                   \
	BMM350_AXIS_OK(inst, 2);                                                                   \
	BUILD_ASSERT((BIT(DT_INST_PROP_BY_IDX(inst, axes, 0)) |                                    \
		      BIT(DT_INST_PROP_BY_IDX(inst, axes, 1)) |                                    \
		      BIT(DT_INST_PROP_BY_IDX(inst, axes, 2))) == 7,                               \
		     "axes names each axis of the chip once");                                     \
                                                                                                   \
	static struct bmm350_data bmm350_data_##inst;                                              \
	static const struct bmm350_config bmm350_config_##inst = {                                 \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.axis_map = DT_INST_PROP(inst, axes),                                              \
		.axis_invert = DT_INST_PROP(inst, axes_inverted),                                  \
	};                                                                                         \
                                                                                                   \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, bmm350_init, NULL, &bmm350_data_##inst,                 \
				     &bmm350_config_##inst, POST_KERNEL,                           \
				     CONFIG_SENSOR_INIT_PRIORITY, &bmm350_api_funcs);

/* Create the struct device for every status "okay" node in the devicetree. */
DT_INST_FOREACH_STATUS_OKAY(BMM350_DEFINE)
