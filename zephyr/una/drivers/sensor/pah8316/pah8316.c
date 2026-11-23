/*
 * Copyright (c) 2026 Albin Stigo
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * PixArt PAH8316LS optical heart-rate sensor on I2C.
 *
 * The part has no public datasheet. This is written from what PixArt's own
 * library was seen to do on the bus (docs/sensors/pah8316-registers.md), and
 * does the same with less:
 *
 * - A setup is written whole, after waking the chip: one for looking for
 *   skin, one for the two green channels at 20 Hz. The chip is never
 *   changed while it runs.
 * - The chip raises its line when skin is found or lost and when 40 entries
 *   wait in its FIFO. A thread reads the flags, the FIFO, and writes the
 *   flags back, which lets the line go.
 * - The LED is the chip's own business. Each sample says what it was taken
 *   with, and is scaled by it.
 * - The supplies are on only between a sampling frequency being set and its
 *   being set to 0.
 */

#define DT_DRV_COMPAT pixart_pah8316

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <una/pah8316.h>

#include "pah8316_setup.h"

LOG_MODULE_REGISTER(una_pah8316, CONFIG_SENSOR_LOG_LEVEL);

#define PRODUCT_ID 0x8311

/* Bank 0 */
#define REG_PRODUCT     0x00
#define REG_RUN         0x07
#define REG_STATUS      0x2f /* two bytes, the flags in the second */
#define REG_FLAGS       0x30 /* the flags again; written back, they clear */
#define REG_TOUCH       0x38
#define REG_FIFO_COUNT  0x44
#define REG_FIFO_ACCESS 0x7a
#define REG_FIFO        0x7b
#define REG_APPLY       0x7e
/* Bank 1 */
#define REG_B1_STOP     0x00
/* Bank 3 */
#define REG_B3_POWER    0x01
#define REG_B3_LOCK     0x0b
#define REG_B3_54       0x54
#define REG_B3_70       0x70

#define FLAG_LOST  BIT(0)
#define FLAG_FIFO  BIT(3)
#define FLAG_SKIN  BIT(4)

#define ENTRY_BYTES 5
#define ENTRY_CH_A  0x08
#define ENTRY_CH_B  0x18
#define ENTRIES_MAX (PAH8316_BATCH_MAX * PAH8316_CHANNELS)

/* As the stock firmware waits. */
#define POWER_ON_MS  10
#define POWER_OFF_MS 20
/* A batch is a second. Three without one, and the chip is started again. */
#define SILENT_MS    3500

enum pah8316_mode {
	MODE_OFF,
	MODE_TOUCH,
	MODE_HR,
};

struct pah8316_config {
	struct i2c_dt_spec bus;
	struct gpio_dt_spec irq;
	struct gpio_dt_spec enable;
	struct gpio_dt_spec led_enable;
};

struct pah8316_data {
	const struct device *dev;
	struct k_mutex lock;
	struct k_sem irq_sem;
	struct gpio_callback irq_cb;
	struct k_thread thread;

	enum pah8316_mode mode;
	bool always;
	bool worn;
	bool fresh;
	int64_t last_batch_ms;
	struct pah8316_batch batch;
	struct pah8316_stats stats;
	uint8_t fifo[ENTRIES_MAX * ENTRY_BYTES];

	sensor_trigger_handler_t batch_handler;
	const struct sensor_trigger *batch_trigger;
	sensor_trigger_handler_t worn_handler;
	const struct sensor_trigger *worn_trigger;
};

static int reg_write(const struct device *dev, uint8_t reg, uint8_t value)
{
	const struct pah8316_config *cfg = dev->config;
	struct pah8316_data *data = dev->data;
	int ret = i2c_reg_write_byte_dt(&cfg->bus, reg, value);

	if (ret != 0) {
		data->stats.bus_errors++;
	}
	return ret;
}

static int reg_read(const struct device *dev, uint8_t reg, uint8_t *buf, size_t len)
{
	const struct pah8316_config *cfg = dev->config;
	struct pah8316_data *data = dev->data;
	int ret = i2c_burst_read_dt(&cfg->bus, reg, buf, len);

	if (ret != 0) {
		data->stats.bus_errors++;
	}
	return ret;
}

static int bank(const struct device *dev, uint8_t n)
{
	return reg_write(dev, PAH8316_REG_BANK, n);
}

static int write_table(const struct device *dev, const uint8_t (*table)[2], size_t rows)
{
	int ret = 0;

	for (size_t i = 0; i < rows && ret == 0; i++) {
		ret = reg_write(dev, table[i][0], table[i][1]);
	}
	return ret;
}

/* Wakes the chip, or with `down` leaves it at the point where it is put down. */
static int wake(const struct device *dev, bool down)
{
	static const uint8_t first[][2] = {
		{PAH8316_REG_BANK, 3}, {REG_B3_LOCK, 0x36}, {REG_B3_54, 0x14},
		{REG_B3_54, 0x10},     {REG_B3_LOCK, 0x37}, {REG_B3_70, 0x10},
	};
	int ret = write_table(dev, first, ARRAY_SIZE(first));

	if (ret != 0) {
		return ret;
	}
	k_msleep(1);
	ret = reg_write(dev, REG_B3_70, 0x02);
	if (ret != 0) {
		return ret;
	}
	k_msleep(1);
	ret = reg_write(dev, REG_B3_POWER, 1);
	if (ret != 0) {
		return ret;
	}
	k_msleep(3);
	if (down) {
		return 0;
	}
	ret = reg_write(dev, REG_B3_POWER, 0);
	k_msleep(15);
	return ret;
}

static int setup(const struct device *dev, enum pah8316_mode mode)
{
	struct pah8316_data *data = dev->data;
	int ret = wake(dev, false);

	if (ret == 0) {
		ret = write_table(dev, pah8316_setup_common, ARRAY_SIZE(pah8316_setup_common));
	}
	if (ret == 0 && mode == MODE_HR) {
		ret = write_table(dev, pah8316_setup_hr, ARRAY_SIZE(pah8316_setup_hr));
	} else if (ret == 0) {
		ret = write_table(dev, pah8316_setup_touch, ARRAY_SIZE(pah8316_setup_touch));
	}
	if (ret == 0) {
		ret = bank(dev, 0);
	}
	if (ret == 0) {
		ret = reg_write(dev, REG_APPLY, 0x80);
	}
	if (ret != 0) {
		return ret;
	}
	k_msleep(1);
	ret = reg_write(dev, REG_RUN, 1);
	if (ret == 0) {
		data->mode = mode;
		data->last_batch_ms = k_uptime_get();
	}
	return ret;
}

static void supplies(const struct device *dev, bool on)
{
	const struct pah8316_config *cfg = dev->config;

	if (on) {
		if (cfg->enable.port != NULL) {
			gpio_pin_set_dt(&cfg->enable, 1);
		}
		if (cfg->led_enable.port != NULL) {
			gpio_pin_set_dt(&cfg->led_enable, 1);
		}
		k_msleep(POWER_ON_MS);
		return;
	}
	if (cfg->led_enable.port != NULL) {
		gpio_pin_set_dt(&cfg->led_enable, 0);
	}
	if (cfg->enable.port != NULL) {
		gpio_pin_set_dt(&cfg->enable, 0);
	}
	k_msleep(POWER_OFF_MS);
}

static int power_up(const struct device *dev)
{
	struct pah8316_data *data = dev->data;
	uint8_t id[2];
	int ret;

	supplies(dev, true);
	ret = wake(dev, false);
	if (ret == 0) {
		ret = bank(dev, 0);
	}
	if (ret == 0) {
		ret = reg_read(dev, REG_PRODUCT, id, sizeof(id));
	}
	if (ret == 0 && sys_get_le16(id) != PRODUCT_ID) {
		LOG_ERR("product %04x is not a PAH8316", sys_get_le16(id));
		ret = -ENODEV;
	}
	if (ret == 0) {
		ret = setup(dev, data->always ? MODE_HR : MODE_TOUCH);
	}
	if (ret != 0) {
		supplies(dev, false);
	}
	return ret;
}

static void set_worn(const struct device *dev, bool worn)
{
	struct pah8316_data *data = dev->data;

	if (worn == data->worn) {
		return;
	}
	data->worn = worn;
	if (data->worn_handler != NULL) {
		data->worn_handler(dev, data->worn_trigger);
	}
}

static int start(const struct device *dev)
{
	const struct pah8316_config *cfg = dev->config;
	struct pah8316_data *data = dev->data;
	int ret;

	if (data->mode != MODE_OFF) {
		return 0;
	}
	data->fresh = false;
	/* Before the chip runs: its line rises within a few ms of that. */
	(void)gpio_pin_interrupt_configure_dt(&cfg->irq, GPIO_INT_EDGE_TO_ACTIVE);
	ret = power_up(dev);
	if (ret != 0) {
		(void)gpio_pin_interrupt_configure_dt(&cfg->irq, GPIO_INT_DISABLE);
	}
	return ret;
}

static void stop(const struct device *dev)
{
	const struct pah8316_config *cfg = dev->config;
	struct pah8316_data *data = dev->data;

	if (data->mode == MODE_OFF) {
		return;
	}
	(void)gpio_pin_interrupt_configure_dt(&cfg->irq, GPIO_INT_DISABLE);
	if (bank(dev, 1) == 0 && reg_write(dev, REG_B1_STOP, 0x98) == 0) {
		(void)wake(dev, true);
	}
	supplies(dev, false);
	data->mode = MODE_OFF;
	set_worn(dev, false);
}

/* The FIFO's entries, made into samples of two channels. */
static void take(const struct device *dev, size_t entries)
{
	struct pah8316_data *data = dev->data;
	struct pah8316_batch *batch = &data->batch;
	uint8_t n = 0;

	for (size_t i = 0; i < entries && n < PAH8316_BATCH_MAX; i++) {
		const uint8_t *entry = &data->fifo[i * ENTRY_BYTES];
		uint8_t led = MAX(entry[0], 1);
		uint64_t light = ((uint64_t)sys_get_le24(&entry[2]) << 8) / led;

		if (entry[1] == ENTRY_CH_A) {
			batch->light[n][0] = MIN(light, UINT32_MAX);
			batch->led[n] = entry[0];
		} else if (entry[1] == ENTRY_CH_B) {
			/* A sample is done with its second channel. */
			batch->light[n][1] = MIN(light, UINT32_MAX);
			n++;
		}
	}
	batch->count = n;
	batch->uptime_ms = k_uptime_get();
	batch->worn = data->worn;
	data->fresh = n > 0;
	data->last_batch_ms = batch->uptime_ms;
	data->stats.batches++;
}

static int read_fifo(const struct device *dev)
{
	struct pah8316_data *data = dev->data;
	uint8_t raw[2];
	size_t entries;
	int ret;

	ret = reg_read(dev, REG_FIFO_COUNT, raw, sizeof(raw));
	if (ret != 0) {
		return ret;
	}
	entries = MIN(sys_get_le16(raw), ENTRIES_MAX);
	if (entries == 0) {
		return 0;
	}
	ret = reg_write(dev, REG_FIFO_ACCESS, 1);
	if (ret == 0) {
		ret = reg_read(dev, REG_FIFO, data->fifo, entries * ENTRY_BYTES);
		(void)reg_write(dev, REG_FIFO_ACCESS, 0);
	}
	if (ret == 0) {
		take(dev, entries);
	}
	return ret;
}

/* One rise of the line. True when a batch came of it. */
static bool serve(const struct device *dev)
{
	struct pah8316_data *data = dev->data;
	uint8_t status[2], flags, touch[2];
	bool batch = false;

	if (bank(dev, 0) != 0 || reg_read(dev, REG_STATUS, status, sizeof(status)) != 0 ||
	    reg_read(dev, REG_FLAGS, &flags, 1) != 0 ||
	    reg_read(dev, REG_TOUCH, touch, sizeof(touch)) != 0) {
		return false;
	}
	data->stats.touch = sys_get_le16(touch);
	if ((flags & FLAG_SKIN) != 0) {
		set_worn(dev, true);
	} else if ((flags & FLAG_LOST) != 0) {
		set_worn(dev, false);
	}
	if ((flags & FLAG_FIFO) != 0 && data->mode == MODE_HR) {
		batch = read_fifo(dev) == 0 && data->fresh;
	}
	(void)reg_write(dev, REG_STATUS, 0);
	(void)reg_write(dev, REG_FLAGS, flags);

	if (!data->always) {
		if (data->worn && data->mode == MODE_TOUCH) {
			(void)setup(dev, MODE_HR);
		} else if (!data->worn && data->mode == MODE_HR) {
			(void)setup(dev, MODE_TOUCH);
		}
	}
	return batch;
}

static void pah8316_irq(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	struct pah8316_data *data = CONTAINER_OF(cb, struct pah8316_data, irq_cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	k_sem_give(&data->irq_sem);
}

static void pah8316_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	const struct pah8316_config *cfg = dev->config;
	struct pah8316_data *data = dev->data;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		bool batch = false;

		(void)k_sem_take(&data->irq_sem, K_MSEC(1000));

		k_mutex_lock(&data->lock, K_FOREVER);
		if (data->mode != MODE_OFF) {
			/* More may have come while one was served, with no edge. */
			for (int i = 0; i < 4 && gpio_pin_get_dt(&cfg->irq) > 0; i++) {
				batch = serve(dev) || batch;
			}
			if (data->mode == MODE_HR &&
			    k_uptime_get() - data->last_batch_ms > SILENT_MS) {
				LOG_WRN("silent, started again");
				data->stats.restarts++;
				supplies(dev, false);
				if (power_up(dev) != 0) {
					data->mode = MODE_OFF;
				}
			}
		}
		k_mutex_unlock(&data->lock);

		if (batch && data->batch_handler != NULL) {
			data->batch_handler(dev, data->batch_trigger);
		}
	}
}

static int pah8316_attr_set(const struct device *dev, enum sensor_channel chan,
			    enum sensor_attribute attr, const struct sensor_value *val)
{
	struct pah8316_data *data = dev->data;
	int ret = 0;

	ARG_UNUSED(chan);

	k_mutex_lock(&data->lock, K_FOREVER);
	if (attr == SENSOR_ATTR_SAMPLING_FREQUENCY) {
		if (val->val1 == 0) {
			stop(dev);
		} else if (val->val1 == PAH8316_SAMPLE_HZ) {
			ret = start(dev);
		} else {
			ret = -EINVAL;
		}
	} else if ((int)attr == PAH8316_ATTR_ALWAYS) {
		if (data->mode != MODE_OFF) {
			ret = -EBUSY;
		} else {
			data->always = val->val1 != 0;
		}
	} else {
		ret = -ENOTSUP;
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static int pah8316_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	struct pah8316_data *data = dev->data;

	ARG_UNUSED(chan);

	/* The thread has fetched already: there is only the last batch. */
	return data->mode == MODE_OFF ? -ENODATA : 0;
}

static int pah8316_channel_get(const struct device *dev, enum sensor_channel chan,
			       struct sensor_value *val)
{
	struct pah8316_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);
	val->val2 = 0;
	if (chan == SENSOR_CHAN_PROX) {
		val->val1 = data->worn;
	} else if (chan == SENSOR_CHAN_GREEN && data->batch.count > 0) {
		const uint32_t *last = data->batch.light[data->batch.count - 1];

		val->val1 = (int32_t)(((uint64_t)last[0] + last[1]) / 2);
	} else {
		ret = chan == SENSOR_CHAN_GREEN ? -ENODATA : -ENOTSUP;
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static int pah8316_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
			       sensor_trigger_handler_t handler)
{
	struct pah8316_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (trig->type == SENSOR_TRIG_FIFO_WATERMARK) {
		data->batch_handler = handler;
		data->batch_trigger = trig;
	} else if ((int)trig->type == PAH8316_TRIG_WORN) {
		data->worn_handler = handler;
		data->worn_trigger = trig;
	} else {
		ret = -ENOTSUP;
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

int pah8316_read_batch(const struct device *dev, struct pah8316_batch *batch)
{
	struct pah8316_data *data = dev->data;
	int ret = -ENODATA;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (data->fresh) {
		*batch = data->batch;
		data->fresh = false;
		ret = 0;
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

void pah8316_stats(const struct device *dev, struct pah8316_stats *stats)
{
	struct pah8316_data *data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	*stats = data->stats;
	k_mutex_unlock(&data->lock);
}

static DEVICE_API(sensor, pah8316_api) = {
	.attr_set = pah8316_attr_set,
	.sample_fetch = pah8316_sample_fetch,
	.channel_get = pah8316_channel_get,
	.trigger_set = pah8316_trigger_set,
};

static K_KERNEL_STACK_ARRAY_DEFINE(pah8316_stacks, DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT),
				   CONFIG_UNA_PAH8316_THREAD_STACK_SIZE);

static int pah8316_init(const struct device *dev)
{
	const struct pah8316_config *cfg = dev->config;
	struct pah8316_data *data = dev->data;
	static uint8_t instance;
	int ret;

	if (!i2c_is_ready_dt(&cfg->bus) || !gpio_is_ready_dt(&cfg->irq)) {
		return -ENODEV;
	}
	/* Without supply from here on. The chip is not asked whether it is there. */
	if (cfg->led_enable.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->led_enable)) {
			return -ENODEV;
		}
		(void)gpio_pin_configure_dt(&cfg->led_enable, GPIO_OUTPUT_INACTIVE);
	}
	if (cfg->enable.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->enable)) {
			return -ENODEV;
		}
		(void)gpio_pin_configure_dt(&cfg->enable, GPIO_OUTPUT_INACTIVE);
	}
	ret = gpio_pin_configure_dt(&cfg->irq, GPIO_INPUT);
	if (ret != 0) {
		return ret;
	}
	data->dev = dev;
	k_mutex_init(&data->lock);
	k_sem_init(&data->irq_sem, 0, 1);
	gpio_init_callback(&data->irq_cb, pah8316_irq, BIT(cfg->irq.pin));
	ret = gpio_add_callback(cfg->irq.port, &data->irq_cb);
	if (ret != 0) {
		return ret;
	}
	k_thread_create(&data->thread, pah8316_stacks[instance],
			K_KERNEL_STACK_SIZEOF(pah8316_stacks[instance]), pah8316_thread,
			(void *)dev, NULL, NULL, K_PRIO_PREEMPT(CONFIG_UNA_PAH8316_THREAD_PRIORITY),
			0, K_NO_WAIT);
	k_thread_name_set(&data->thread, "pah8316");
	instance++;
	return 0;
}

#define PAH8316_DEFINE(inst)                                                                       \
	static struct pah8316_data pah8316_data_##inst;                                            \
	static const struct pah8316_config pah8316_config_##inst = {                               \
		.bus = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.irq = GPIO_DT_SPEC_INST_GET(inst, irq_gpios),                                     \
		.enable = GPIO_DT_SPEC_INST_GET_OR(inst, enable_gpios, {0}),                       \
		.led_enable = GPIO_DT_SPEC_INST_GET_OR(inst, led_enable_gpios, {0}),               \
	};                                                                                         \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, pah8316_init, NULL, &pah8316_data_##inst,               \
				     &pah8316_config_##inst, POST_KERNEL,                          \
				     CONFIG_SENSOR_INIT_PRIORITY, &pah8316_api);

DT_INST_FOREACH_STATUS_OKAY(PAH8316_DEFINE)
