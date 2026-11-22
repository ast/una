/*
 * Copyright (c) 2024 Bosch Sensortec GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef UNA_DRIVERS_SENSOR_BMM350_BMM350_H_
#define UNA_DRIVERS_SENSOR_BMM350_BMM350_H_

#include <zephyr/sys/util.h>

#define BMM350_REG_CHIP_ID          0x00
#define BMM350_REG_ERR_REG          0x02
#define BMM350_REG_PMU_CMD_AGGR_SET 0x04
#define BMM350_REG_PMU_CMD          0x06
#define BMM350_REG_PMU_CMD_STATUS_0 0x07
#define BMM350_REG_MAG_X_XLSB       0x31
#define BMM350_REG_OTP_CMD_REG      0x50
#define BMM350_REG_OTP_DATA_MSB_REG 0x52
#define BMM350_REG_OTP_DATA_LSB_REG 0x53
#define BMM350_REG_OTP_STATUS_REG   0x55
#define BMM350_REG_CMD              0x7E

#define BMM350_CHIP_ID       0x33
#define BMM350_CMD_SOFTRESET 0xB6

/* Every read begins with two bytes that mean nothing. */
#define BMM350_DUMMY_BYTES 2

#define BMM350_OTP_CMD_DIR_READ      0x20
#define BMM350_OTP_CMD_PWR_OFF_OTP   0x80
#define BMM350_OTP_WORD_ADDR_MSK     0x1F
#define BMM350_OTP_STATUS_ERROR_MSK  0xE0
#define BMM350_OTP_STATUS_CMD_DONE   0x01
#define BMM350_OTP_DATA_LENGTH       32
/* Reads of the status before a word is given up. */
#define BMM350_OTP_TRIES             50

/* The words of the OTP that hold the trim. */
#define BMM350_TEMP_OFF_SENS 0x0D
#define BMM350_MAG_OFFSET_X  0x0E
#define BMM350_MAG_OFFSET_Y  0x0F
#define BMM350_MAG_OFFSET_Z  0x10
#define BMM350_MAG_SENS_X    0x10
#define BMM350_MAG_SENS_Y    0x11
#define BMM350_MAG_SENS_Z    0x11
#define BMM350_MAG_TCO_X     0x12
#define BMM350_MAG_TCO_Y     0x13
#define BMM350_MAG_TCO_Z     0x14
#define BMM350_MAG_TCS_X     0x12
#define BMM350_MAG_TCS_Y     0x13
#define BMM350_MAG_TCS_Z     0x14
#define BMM350_MAG_DUT_T_0   0x18
#define BMM350_CROSS_X_Y     0x15
#define BMM350_CROSS_Y_X     0x15
#define BMM350_CROSS_Z_X     0x16
#define BMM350_CROSS_Z_Y     0x16

#define BMM350_PMU_CMD_SUS     0x00
#define BMM350_PMU_CMD_NM      0x01
#define BMM350_PMU_CMD_UPD_OAE 0x02
#define BMM350_PMU_CMD_FGR     0x05
#define BMM350_PMU_CMD_BR      0x07

/* PMU_CMD_STATUS_0: the last command done, in its top three bits. */
#define BMM350_PMU_CMD_VALUE_POS       5
#define BMM350_PMU_CMD_STATUS_0_FGR    0x05
#define BMM350_PMU_CMD_STATUS_0_BR     0x07

/* PMU_CMD_AGGR_SET */
#define BMM350_ODR_400HZ    0x02
#define BMM350_ODR_200HZ    0x03
#define BMM350_ODR_100HZ    0x04
#define BMM350_ODR_50HZ     0x05
#define BMM350_ODR_25HZ     0x06
#define BMM350_ODR_12_5HZ   0x07
#define BMM350_ODR_6_25HZ   0x08
#define BMM350_ODR_3_125HZ  0x09
#define BMM350_ODR_1_5625HZ 0x0A
#define BMM350_AVG_POS      4
#define BMM350_AVG_1        0
#define BMM350_AVG_2        1
#define BMM350_AVG_4        2
#define BMM350_AVG_8        3

/* Microseconds */
#define BMM350_SOFT_RESET_DELAY        24000
#define BMM350_GOTO_SUSPEND_DELAY      6000
#define BMM350_SUSPEND_TO_NORMAL_DELAY 38000
#define BMM350_UPD_OAE_DELAY           1000
#define BMM350_BR_DELAY                14000
#define BMM350_FGR_DELAY               18000

#endif /* UNA_DRIVERS_SENSOR_BMM350_BMM350_H_ */
