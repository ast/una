/*
 * Copyright (c) 2023 Elektronikutvecklingsbyrån EUB AB
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The chip's interrupts: all of them on one line, latched, and told apart
 * by the two status registers, which are read in a thread.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <una/bmi270.h>

#include "bmi270.h"

LOG_MODULE_DECLARE(una_bmi270, CONFIG_SENSOR_LOG_LEVEL);

/* Where each event shows: INT_STATUS_0, or INT_STATUS_1 for data ready. */
static const uint8_t status_bits[BMI270_EVENT_COUNT] = {
	[BMI270_EVENT_DRDY] = BMI270_INT_STATUS_1_ACC_DRDY | BMI270_INT_STATUS_1_GYR_DRDY,
	[BMI270_EVENT_ANY_MOTION] = BMI270_INT_ANY_MOTION,
	[BMI270_EVENT_NO_MOTION] = BMI270_INT_NO_MOTION,
	[BMI270_EVENT_STEP] = BMI270_INT_STEP_COUNTER,
	[BMI270_EVENT_ACTIVITY] = BMI270_INT_ACTIVITY,
	[BMI270_EVENT_WRIST_WAKE] = BMI270_INT_WRIST_WEAR_WAKEUP,
};

static void bmi270_irq_callback(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	struct bmi270_data *data = CONTAINER_OF(cb, struct bmi270_data, irq_cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	k_sem_give(&data->irq_sem);
}

static void bmi270_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	const struct bmi270_config *cfg = dev->config;
	struct bmi270_data *data = dev->data;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_sem_take(&data->irq_sem, K_FOREVER);

		/*
		 * Reading the status lets the line go, and it rises again for
		 * what comes after. What came between the two has no edge to
		 * show for it: hence the look at the line.
		 */
		for (int round = 0; round < 8; round++) {
			struct bmi270_handler handlers[BMI270_EVENT_COUNT];
			uint8_t status[2];
			int ret;

			k_mutex_lock(&data->lock, K_FOREVER);
			ret = bmi270_reg_read(dev, BMI270_REG_INT_STATUS_0, status,
					      sizeof(status));
			memcpy(handlers, data->handlers, sizeof(handlers));
			k_mutex_unlock(&data->lock);
			if (ret != 0) {
				LOG_ERR("read interrupt status returned %d", ret);
				break;
			}

			for (int event = 0; event < BMI270_EVENT_COUNT; event++) {
				uint8_t bits = status[event == BMI270_EVENT_DRDY ? 1 : 0];

				if ((bits & status_bits[event]) != 0 &&
				    handlers[event].handler != NULL) {
					handlers[event].handler(dev, handlers[event].trigger);
				}
			}

			if (gpio_pin_get_dt(&cfg->irq) <= 0) {
				break;
			}
		}
	}
}

int bmi270_init_interrupts(const struct device *dev)
{
	const struct bmi270_config *cfg = dev->config;
	struct bmi270_data *data = dev->data;
	uint8_t io_ctrl = BMI270_INT_IO_CTRL_OUTPUT_EN;
	uint8_t status[2];
	int ret;

	if (cfg->irq.port == NULL) {
		return 0;
	}

	if (!gpio_is_ready_dt(&cfg->irq)) {
		LOG_ERR_DEVICE_NOT_READY(cfg->irq.port);
		return -ENODEV;
	}

	if ((cfg->irq.dt_flags & GPIO_ACTIVE_LOW) == 0) {
		io_ctrl |= BMI270_INT_IO_CTRL_LVL;
	}
	ret = bmi270_reg_write(dev,
			       cfg->int_pin == 1 ? BMI270_REG_INT1_IO_CTRL
						 : BMI270_REG_INT2_IO_CTRL,
			       io_ctrl);
	if (ret == 0) {
		ret = bmi270_reg_write(dev, BMI270_REG_INT_LATCH, BMI270_INT_LATCH_PERMANENT);
	}
	if (ret == 0) {
		/* Nothing is on the line yet; this is for what the reset left. */
		ret = bmi270_reg_read(dev, BMI270_REG_INT_STATUS_0, status, sizeof(status));
	}
	if (ret != 0) {
		LOG_ERR("failed configuring INT%d (%d)", cfg->int_pin, ret);
		return ret;
	}

	k_sem_init(&data->irq_sem, 0, 1);
	k_thread_create(&data->thread, data->thread_stack, CONFIG_UNA_BMI270_THREAD_STACK_SIZE,
			bmi270_thread, (void *)dev, NULL, NULL,
			K_PRIO_PREEMPT(CONFIG_UNA_BMI270_THREAD_PRIORITY), 0, K_NO_WAIT);
	k_thread_name_set(&data->thread, "bmi270");

	gpio_init_callback(&data->irq_cb, bmi270_irq_callback, BIT(cfg->irq.pin));

	ret = gpio_pin_configure_dt(&cfg->irq, GPIO_INPUT);
	if (ret == 0) {
		ret = gpio_add_callback(cfg->irq.port, &data->irq_cb);
	}
	if (ret == 0) {
		ret = gpio_pin_interrupt_configure_dt(&cfg->irq, GPIO_INT_EDGE_TO_ACTIVE);
	}
	return ret;
}

int bmi270_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
		       sensor_trigger_handler_t handler)
{
	const struct bmi270_config *cfg = dev->config;
	struct bmi270_data *data = dev->data;
	enum bmi270_event event;
	uint8_t feature = 0;
	int ret = 0;

	if (cfg->irq.port == NULL) {
		return -ENOTSUP;
	}

	switch ((int)trig->type) {
	case SENSOR_TRIG_DATA_READY:
		event = BMI270_EVENT_DRDY;
		break;
	case SENSOR_TRIG_MOTION:
		event = BMI270_EVENT_ANY_MOTION;
		feature = BMI270_FEATURE_ANY_MOTION;
		break;
	case SENSOR_TRIG_STATIONARY:
		event = BMI270_EVENT_NO_MOTION;
		feature = BMI270_FEATURE_NO_MOTION;
		break;
	case BMI270_TRIG_STEP:
		event = BMI270_EVENT_STEP;
		break;
	case BMI270_TRIG_ACTIVITY:
		event = BMI270_EVENT_ACTIVITY;
		break;
	case BMI270_TRIG_WRIST_WAKE:
		event = BMI270_EVENT_WRIST_WAKE;
		feature = BMI270_FEATURE_WRIST_WAKE;
		break;
	default:
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	data->handlers[event].handler = handler;
	data->handlers[event].trigger = trig;

	if (feature != 0) {
		ret = bmi270_feature_set(dev, feature, handler != NULL);
	}
	if (ret == 0 && event == BMI270_EVENT_DRDY) {
		uint8_t map = cfg->int_pin == 1 ? BMI270_INT_MAP_DATA_DRDY_INT1
						: BMI270_INT_MAP_DATA_DRDY_INT2;

		ret = bmi270_reg_write(dev, BMI270_REG_INT_MAP_DATA, handler != NULL ? map : 0);
	} else if (ret == 0) {
		WRITE_BIT(data->int_map, find_lsb_set(status_bits[event]) - 1, handler != NULL);
		ret = bmi270_reg_write(dev,
				       cfg->int_pin == 1 ? BMI270_REG_INT1_MAP_FEAT
							 : BMI270_REG_INT2_MAP_FEAT,
				       data->int_map);
	}
	if (ret != 0) {
		data->handlers[event].handler = NULL;
	}
	k_mutex_unlock(&data->lock);

	return ret;
}
