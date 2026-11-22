/*
 * Copyright (c) 2021 Bosch Sensortec GmbH
 * Copyright (c) 2022 Nordic Semiconductor ASA
 * Copyright (c) 2026 Infineon Technologies AG,
 * or an affiliate of Infineon Technologies AG.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef UNA_DRIVERS_SENSOR_BMI270_BMI270_H_
#define UNA_DRIVERS_SENSOR_BMI270_BMI270_H_

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define BMI270_REG_CHIP_ID         0x00
#define BMI270_REG_ERROR           0x02
#define BMI270_REG_STATUS          0x03
#define BMI270_REG_ACC_X_LSB       0x0C
#define BMI270_REG_GYR_X_LSB       0x12
#define BMI270_REG_INT_STATUS_0    0x1C
#define BMI270_REG_INT_STATUS_1    0x1D
#define BMI270_REG_SC_OUT_0        0x1E
#define BMI270_REG_WR_GEST_ACT     0x20
#define BMI270_REG_INTERNAL_STATUS 0x21
#define BMI270_REG_TEMPERATURE_0   0x22
#define BMI270_REG_FEAT_PAGE       0x2F
#define BMI270_REG_FEATURES_0      0x30
#define BMI270_REG_ACC_CONF        0x40
#define BMI270_REG_ACC_RANGE       0x41
#define BMI270_REG_GYR_CONF        0x42
#define BMI270_REG_GYR_RANGE       0x43
#define BMI270_REG_INT1_IO_CTRL    0x53
#define BMI270_REG_INT2_IO_CTRL    0x54
#define BMI270_REG_INT_LATCH       0x55
#define BMI270_REG_INT1_MAP_FEAT   0x56
#define BMI270_REG_INT2_MAP_FEAT   0x57
#define BMI270_REG_INT_MAP_DATA    0x58
#define BMI270_REG_INIT_CTRL       0x59
#define BMI270_REG_INIT_ADDR_0     0x5B
#define BMI270_REG_INIT_DATA       0x5E
#define BMI270_REG_PWR_CONF        0x7C
#define BMI270_REG_PWR_CTRL        0x7D
#define BMI270_REG_CMD             0x7E

#define BMI270_CHIP_ID        0x24
#define BMI270_CMD_SOFT_RESET 0xB6

/* Microseconds */
#define BMI270_POWER_ON_TIME   500
#define BMI270_SOFT_RESET_TIME 2000
/* After a write, with advanced power save on and off (datasheet 6.4). */
#define BMI270_WRITE_IDLE_SUSPEND 450
#define BMI270_WRITE_IDLE_NORMAL  2

/* The configuration goes up in pieces of this many bytes, an even number. */
#define BMI270_CONFIG_FILE_CHUNK          256
#define BMI270_CONFIG_FILE_RETRIES        15
#define BMI270_CONFIG_FILE_POLL_PERIOD_US 10000

#define BMI270_PREPARE_CONFIG_LOAD  0x00
#define BMI270_COMPLETE_CONFIG_LOAD 0x01

#define BMI270_INST_MESSAGE_MSK      0x0F
#define BMI270_INST_MESSAGE_INIT_OK  0x01
#define BMI270_INST_AXES_REMAP_ERROR 0x20
#define BMI270_INST_ODR_50HZ_ERROR   0x40

/* PWR_CONF: bit 1, the FIFO waking the chip, is left as a reset sets it. */
#define BMI270_PWR_CONF_ADV_PWR_SAVE_EN  0x03
#define BMI270_PWR_CONF_ADV_PWR_SAVE_DIS 0x02

#define BMI270_PWR_CTRL_GYR_EN  0x02
#define BMI270_PWR_CTRL_ACC_EN  0x04
#define BMI270_PWR_CTRL_TEMP_EN 0x08

#define BMI270_ACC_ODR_25D32_HZ 0x01
#define BMI270_ACC_ODR_25D16_HZ 0x02
#define BMI270_ACC_ODR_25D8_HZ  0x03
#define BMI270_ACC_ODR_25D4_HZ  0x04
#define BMI270_ACC_ODR_25D2_HZ  0x05
#define BMI270_ACC_ODR_25_HZ    0x06
#define BMI270_ACC_ODR_50_HZ    0x07
#define BMI270_ACC_ODR_100_HZ   0x08
#define BMI270_ACC_ODR_200_HZ   0x09
#define BMI270_ACC_ODR_400_HZ   0x0A
#define BMI270_ACC_ODR_800_HZ   0x0B
#define BMI270_ACC_ODR_1600_HZ  0x0C

#define BMI270_ACC_BWP_POS        4
#define BMI270_ACC_BWP_OSR4_AVG1  0x00
#define BMI270_ACC_BWP_OSR2_AVG2  0x01
#define BMI270_ACC_BWP_NORM_AVG4  0x02
#define BMI270_ACC_BWP_CIC_AVG8   0x03
#define BMI270_ACC_BWP_RES_AVG16  0x04
#define BMI270_ACC_BWP_RES_AVG32  0x05
#define BMI270_ACC_BWP_RES_AVG64  0x06
#define BMI270_ACC_BWP_RES_AVG128 0x07

#define BMI270_ACC_FILT_PERF_OPT BIT(7)

#define BMI270_ACC_RANGE_2G  0x00
#define BMI270_ACC_RANGE_4G  0x01
#define BMI270_ACC_RANGE_8G  0x02
#define BMI270_ACC_RANGE_16G 0x03

#define BMI270_GYR_ODR_25_HZ   0x06
#define BMI270_GYR_ODR_50_HZ   0x07
#define BMI270_GYR_ODR_100_HZ  0x08
#define BMI270_GYR_ODR_200_HZ  0x09
#define BMI270_GYR_ODR_400_HZ  0x0A
#define BMI270_GYR_ODR_800_HZ  0x0B
#define BMI270_GYR_ODR_1600_HZ 0x0C
#define BMI270_GYR_ODR_3200_HZ 0x0D

#define BMI270_GYR_BWP_POS  4
#define BMI270_GYR_BWP_OSR4 0x00
#define BMI270_GYR_BWP_OSR2 0x01
#define BMI270_GYR_BWP_NORM 0x02

#define BMI270_GYR_FILT_NOISE_PERF BIT(6)
#define BMI270_GYR_FILT_PERF_OPT   BIT(7)

#define BMI270_GYR_RANGE_2000DPS 0x00
#define BMI270_GYR_RANGE_1000DPS 0x01
#define BMI270_GYR_RANGE_500DPS  0x02
#define BMI270_GYR_RANGE_250DPS  0x03
#define BMI270_GYR_RANGE_125DPS  0x04

#define BMI270_TEMP_INVALID       0x8000
#define BMI270_TEMP_CENTER_OFFSET 23000000LL
#define BMI270_TEMP_LSB_PER_DEG   512LL

/* INT1_IO_CTRL, INT2_IO_CTRL */
#define BMI270_INT_IO_CTRL_LVL       BIT(1) /* active high */
#define BMI270_INT_IO_CTRL_OD        BIT(2) /* open drain */
#define BMI270_INT_IO_CTRL_OUTPUT_EN BIT(3)

#define BMI270_INT_LATCH_PERMANENT 0x01

/* INT1_MAP_FEAT, INT2_MAP_FEAT and INT_STATUS_0 */
#define BMI270_INT_SIG_MOTION        BIT(0)
#define BMI270_INT_STEP_COUNTER      BIT(1)
#define BMI270_INT_ACTIVITY          BIT(2)
#define BMI270_INT_WRIST_WEAR_WAKEUP BIT(3)
#define BMI270_INT_WRIST_GESTURE     BIT(4)
#define BMI270_INT_NO_MOTION         BIT(5)
#define BMI270_INT_ANY_MOTION        BIT(6)

#define BMI270_INT_MAP_DATA_DRDY_INT1 BIT(2)
#define BMI270_INT_MAP_DATA_DRDY_INT2 BIT(6)

/* INT_STATUS_1 */
#define BMI270_INT_STATUS_1_GYR_DRDY BIT(6)
#define BMI270_INT_STATUS_1_ACC_DRDY BIT(7)

/* WR_GEST_ACT */
#define BMI270_WR_GEST_ACT_ACT_POS 3
#define BMI270_WR_GEST_ACT_ACT_MSK 0x03

/*
 * The feature registers: sixteen bytes at 0x30 that show the page chosen in
 * FEAT_PAGE. Each register is a 16-bit word (datasheet 5.2.38).
 */
#define BMI270_FEAT_PAGE_GEN_SET_1 1
#define BMI270_FEAT_GEN_SET_1      0x34
/* x, its sign, y, its sign, z; the sign of z is the bit above. */
#define BMI270_GEN_SET_1_AXES_MSK  BIT_MASK(9)

#define BMI270_FEAT_PAGE_ANYMO 1
#define BMI270_FEAT_ANYMO_1    0x3C
#define BMI270_FEAT_ANYMO_2    0x3E
#define BMI270_FEAT_PAGE_NOMO  2
#define BMI270_FEAT_NOMO_2     0x32

/* ANYMO_1: for how many samples at 50 Hz, on which axes */
#define BMI270_ANYMO_1_DURATION_MSK BIT_MASK(13)
#define BMI270_ANYMO_1_SELECT_XYZ   (BIT(13) | BIT(14) | BIT(15))
/* ANYMO_2 and NOMO_2: a slope of how much, 1 g being 2048, and the enable */
#define BMI270_ANYMO_2_THRESHOLD_MSK BIT_MASK(11)
#define BMI270_ANYMO_2_ENABLE        BIT(15)

#define BMI270_FEAT_PAGE_SC_26       6
#define BMI270_FEAT_SC_26            0x32
#define BMI270_SC_26_WATERMARK_MSK   BIT_MASK(10)
#define BMI270_SC_26_RESET_COUNTER   BIT(10)
#define BMI270_SC_26_EN_DETECTOR     BIT(11)
#define BMI270_SC_26_EN_COUNTER      BIT(12)
#define BMI270_SC_26_EN_ACTIVITY     BIT(13)
#define BMI270_SC_26_WATERMARK_STEPS 20

#define BMI270_FEAT_PAGE_WR_WAKEUP 7
#define BMI270_FEAT_WR_WAKEUP_1    0x30
#define BMI270_WR_WAKEUP_1_ENABLE  BIT(4)
#define BMI270_FEAT_WR_WAKEUP_2    0x32
#define BMI270_WR_WAKEUP_PARAMS    6

/* Sensors of the chip that run on the accelerometer, bits of data->features. */
enum {
	BMI270_FEATURE_STEPS = BIT(0),
	BMI270_FEATURE_STEP_DETECTOR = BIT(1),
	BMI270_FEATURE_ACTIVITY = BIT(2),
	BMI270_FEATURE_WRIST_WAKE = BIT(3),
	BMI270_FEATURE_ANY_MOTION = BIT(4),
	BMI270_FEATURE_NO_MOTION = BIT(5),
};

/* What may have a handler. */
enum bmi270_event {
	BMI270_EVENT_DRDY,
	BMI270_EVENT_ANY_MOTION,
	BMI270_EVENT_NO_MOTION,
	BMI270_EVENT_STEP,
	BMI270_EVENT_ACTIVITY,
	BMI270_EVENT_WRIST_WAKE,
	BMI270_EVENT_COUNT,
};

struct bmi270_handler {
	sensor_trigger_handler_t handler;
	const struct sensor_trigger *trigger;
};

struct bmi270_data {
	/* Every sequence on the bus, and what is below. */
	struct k_mutex lock;

	int16_t acc[3], gyr[3], temp;
	uint8_t acc_range;
	uint16_t gyr_range;
	/* As asked for: 0 is off. */
	uint8_t acc_odr, acc_bwp, gyr_odr, gyr_bwp;
	uint8_t features;

	/* The chip's count is read as 16 bits; these make 32 of it. */
	uint32_t steps;
	uint16_t steps_raw;
	uint8_t activity;

	uint16_t anymo_1, anymo_2;

	/* The features whose interrupts are on the line. */
	uint8_t int_map;

	const struct device *dev;
	struct gpio_callback irq_cb;
	struct k_sem irq_sem;
	struct bmi270_handler handlers[BMI270_EVENT_COUNT];
	struct k_thread thread;
	K_KERNEL_STACK_MEMBER(thread_stack, CONFIG_UNA_BMI270_THREAD_STACK_SIZE);
};

struct bmi270_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec irq;
	uint8_t int_pin;
	uint8_t axis_map[3];
	uint8_t axis_invert[3];
	uint16_t wrist_wake[BMI270_WR_WAKEUP_PARAMS];
	bool wrist_wake_given;
};

/* All of these with data->lock held. */
int bmi270_reg_read(const struct device *dev, uint8_t reg, uint8_t *data, uint16_t length);
int bmi270_reg_write(const struct device *dev, uint8_t reg, uint8_t value);

/*
 * The feature registers are reached between these two, which turn advanced
 * power save off and on again.
 */
int bmi270_feat_begin(const struct device *dev);
int bmi270_feat_end(const struct device *dev);
int bmi270_feat_read(const struct device *dev, uint8_t page, uint8_t reg, uint16_t *value);
int bmi270_feat_write(const struct device *dev, uint8_t page, uint8_t reg, uint16_t value);
int bmi270_feat_update(const struct device *dev, uint8_t page, uint8_t reg, uint16_t mask,
		       uint16_t value);

/* Turn one of the BMI270_FEATURE_* on or off, and the accelerometer with them. */
int bmi270_feature_set(const struct device *dev, uint8_t feature, bool on);

int bmi270_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
		       sensor_trigger_handler_t handler);
int bmi270_init_interrupts(const struct device *dev);

#endif /* UNA_DRIVERS_SENSOR_BMI270_BMI270_H_ */
