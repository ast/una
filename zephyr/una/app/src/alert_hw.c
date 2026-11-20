/*
 * The buzzer and the vibration motor.
 *
 * Buzzer: a piezo on TIM4 channel 3 behind two enable pins that set its
 * level, as in Components/Buzzer.cpp of una-kernel-public. Motor: an ERM on a
 * DRV2625, driven open-loop in real-time mode, which needs none of the
 * calibration the stock firmware runs; the chip is in shutdown otherwise.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/policy.h>

#include "alert.h"

LOG_MODULE_REGISTER(alert_hw, LOG_LEVEL_INF);

#define USER DT_PATH(zephyr_user)

static const struct pwm_dt_spec buzzer = PWM_DT_SPEC_GET(USER);
static const struct gpio_dt_spec buzzer_en1 = GPIO_DT_SPEC_GET_BY_IDX(USER, buzzer_en_gpios, 0);
static const struct gpio_dt_spec buzzer_en2 = GPIO_DT_SPEC_GET_BY_IDX(USER, buzzer_en_gpios, 1);
static const struct gpio_dt_spec motor_en = GPIO_DT_SPEC_GET(USER, haptic_enable_gpios);
static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c1));

#define DRV2625_ADDR 0x5a

#define DRV2625_MODE      0x07 /* [1:0] mode, 0 = real-time; [3:2] trigger pin */
#define DRV2625_LOOP      0x08
#define DRV2625_GO        0x0c
#define DRV2625_RTP_INPUT 0x0e
#define DRV2625_OD_CLAMP  0x20

/* ERM, open loop, brake into standby when stopped. */
#define DRV2625_LOOP_ERM_OPEN (BIT(6) | BIT(3))
/* 2.95 V, the stock firmware's limit for this motor. */
#define DRV2625_OD_CLAMP_2V95 137

void buzzer_set(bool on)
{
	static bool is_on;

	if (on == is_on) {
		return;
	}
	is_on = on;
	if (on) {
		/* The timer stands still in Stop mode. */
		pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
		/* Both enables: the loudest of the three levels. */
		gpio_pin_set_dt(&buzzer_en1, 1);
		gpio_pin_set_dt(&buzzer_en2, 1);
		pwm_set_pulse_dt(&buzzer, buzzer.period / 2);
	} else {
		pwm_set_pulse_dt(&buzzer, 0);
		gpio_pin_set_dt(&buzzer_en1, 0);
		gpio_pin_set_dt(&buzzer_en2, 0);
		pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
	}
}

void motor_set(bool on)
{
	static bool is_on;
	int err;

	if (on == is_on) {
		return;
	}
	is_on = on;
	if (!on) {
		i2c_reg_write_byte(i2c, DRV2625_ADDR, DRV2625_GO, 0);
		gpio_pin_set_dt(&motor_en, 0);
		return;
	}
	/* Out of shutdown the registers are at their defaults. */
	gpio_pin_set_dt(&motor_en, 1);
	k_msleep(2);
	err = i2c_reg_write_byte(i2c, DRV2625_ADDR, DRV2625_LOOP, DRV2625_LOOP_ERM_OPEN);
	if (err == 0) {
		err = i2c_reg_write_byte(i2c, DRV2625_ADDR, DRV2625_OD_CLAMP,
					 DRV2625_OD_CLAMP_2V95);
	}
	if (err == 0) {
		err = i2c_reg_update_byte(i2c, DRV2625_ADDR, DRV2625_MODE, 0x0f, 0);
	}
	if (err == 0) {
		err = i2c_reg_write_byte(i2c, DRV2625_ADDR, DRV2625_RTP_INPUT, 0x7f);
	}
	if (err == 0) {
		err = i2c_reg_write_byte(i2c, DRV2625_ADDR, DRV2625_GO, 1);
	}
	if (err != 0) {
		LOG_ERR("motor driver: %d", err);
	}
}

static int alert_hw_init(void)
{
	gpio_pin_configure_dt(&buzzer_en1, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&buzzer_en2, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&motor_en, GPIO_OUTPUT_INACTIVE);
	return 0;
}
SYS_INIT(alert_hw_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
