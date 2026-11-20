/*
 * Sharp LS012B7DD06A memory LCD on STM32U5 GPIO.
 *
 * Follows Libs/Source/Hardware/LS012B7DD06A.cpp in una-kernel-public: the
 * same signal sequence and the same microsecond delays. That file is built
 * unoptimised and gets its sub-microsecond timing from that; here the data
 * and clock edges are spaced with explicit delays instead.
 */

#define DT_DRV_COMPAT sharp_ls012b7dd06a

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <soc.h>
#include <stm32_ll_bus.h>
#include <stm32_ll_rcc.h>

LOG_MODULE_REGISTER(ls012b7dd06a, CONFIG_DISPLAY_LOG_LEVEL);

#define NODE   DT_DRV_INST(0)
#define WIDTH  DT_INST_PROP(0, width)
#define HEIGHT DT_INST_PROP(0, height)

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "one panel only");
BUILD_ASSERT(WIDTH == 240 && HEIGHT == 240);

#define RGB_PIN(i) DT_GPIO_PIN_BY_IDX(NODE, rgb_gpios, i)
BUILD_ASSERT(RGB_PIN(0) == 0 && RGB_PIN(1) == 8 && RGB_PIN(2) == 2 && RGB_PIN(3) == 10 &&
	     RGB_PIN(4) == 4 && RGB_PIN(5) == 12,
	     "rgb-gpios must be pins 0, 8, 2, 10, 4, 12");

#define PORT(prop)    ((GPIO_TypeDef *)DT_REG_ADDR(DT_GPIO_CTLR(NODE, prop)))
#define HIGH(prop)    (PORT(prop)->BSRR = BIT(DT_GPIO_PIN(NODE, prop)))
#define LOW(prop)     (PORT(prop)->BRR = BIT(DT_GPIO_PIN(NODE, prop)))
#define RGB_WRITE(v)  (PORT(rgb_gpios)->ODR = (v))

/* 30 Hz from the 32.768 kHz LSE; the panel allows up to 60 Hz. */
#define FRP_PERIOD (32768 / 30)

/*
 * Data setup and hold around a BCK edge are 220 ns each. At 160 MHz this is
 * a little over that, and it does not depend on the optimisation level.
 */
#define EDGE_DELAY() __asm__ volatile(".rept 36\n nop\n .endr")

/*
 * Microsecond delays from the core's cycle counter. delay_us() cannot be
 * used: with the low-power timer as system clock its resolution is 30 us.
 */
static inline void delay_us(uint32_t us)
{
	const uint32_t start = DWT->CYCCNT;
	const uint32_t cycles = us * (DT_PROP(DT_NODELABEL(rcc), clock_frequency) / 1000000U);

	while (DWT->CYCCNT - start < cycles) {
	}
}

struct ls012_data {
	LPTIM_HandleTypeDef frp;
	LPTIM_HandleTypeDef xfrp;
	struct k_mutex lock;
	/*
	 * What the panel shows, in panel scan order: one byte per pixel,
	 * 0b00BBGGRR. Halfword aligned because a pixel pair is read at a time.
	 */
	uint8_t fb[WIDTH * HEIGHT] __aligned(4);
};

struct ls012_config {
	const struct pinctrl_dev_config *pcfg;
	struct gpio_dt_spec ctrl[6];
	struct gpio_dt_spec rgb[6];
	bool rotated;
};

/* An application pixel's place in the panel's scan order. */
static inline uint32_t fb_index(const struct ls012_config *cfg, uint32_t x, uint32_t y)
{
	return cfg->rotated ? (WIDTH - 1 - x) * WIDTH + y : y * WIDTH + x;
}

static inline uint8_t from_rgb565(uint16_t px)
{
	return ((px >> 14) & 0x3) | (((px >> 9) & 0x3) << 2) | (((px >> 3) & 0x3) << 4);
}

static inline uint16_t to_rgb565(uint8_t v)
{
	/* 2 bits spread over 5 or 6: 0, 1/3, 2/3, full. */
	uint16_t r = (v & 0x3) * 31 / 3;
	uint16_t g = ((v >> 2) & 0x3) * 63 / 3;
	uint16_t b = ((v >> 4) & 0x3) * 31 / 3;

	return (r << 11) | (g << 5) | b;
}

/*
 * One half of a line: the upper or the lower bit of every colour. Takes at
 * least the 68.89 us that GCK has to stay at one level.
 */
static void write_bitplane(const uint8_t *line, bool msb)
{
	const uint16_t *pair = (const uint16_t *)line;
	const uint32_t shift = msb ? 1 : 0;

	HIGH(hst_gpios);
	EDGE_DELAY();
	HIGH(hck_gpios);
	delay_us(1); /* without it the first pixels of the line are lost */
	LOW(hck_gpios);
	EDGE_DELAY();
	LOW(hst_gpios);
	EDGE_DELAY();

	for (int i = 0; i < WIDTH / 4; i++) {
		RGB_WRITE(*pair++ >> shift);
		EDGE_DELAY();
		HIGH(hck_gpios);
		EDGE_DELAY();
		RGB_WRITE(*pair++ >> shift);
		EDGE_DELAY();
		LOW(hck_gpios);
		EDGE_DELAY();
	}

	HIGH(hck_gpios);
	HIGH(enb_gpios);
	EDGE_DELAY();
	EDGE_DELAY();
	LOW(hck_gpios);

	delay_us(11); /* twhGEN 10.64 us */
	LOW(enb_gpios);
	delay_us(5); /* thhGCK 4.4 us */
}

static void refresh(struct ls012_data *data)
{
	const uint8_t *line = data->fb;

	/* As the stock kernel does: no other thread, interrupts stay on. */
	k_sched_lock();

	HIGH(xrst_gpios);
	delay_us(11); /* thsINTB */
	HIGH(vst_gpios);
	delay_us(11); /* thsGSP */
	HIGH(vck_gpios);
	delay_us(69); /* thwGCK 68.89 us */

	LOW(vck_gpios);
	write_bitplane(line, true);
	LOW(vst_gpios);
	delay_us(11); /* tlsGSP */
	HIGH(vck_gpios);
	write_bitplane(line, false);

	for (int y = 1; y < HEIGHT; y++) {
		line += WIDTH;
		LOW(vck_gpios);
		write_bitplane(line, true);
		HIGH(vck_gpios);
		write_bitplane(line, false);
	}

	LOW(vck_gpios);
	delay_us(69);
	HIGH(vck_gpios);
	delay_us(69);
	LOW(vck_gpios);
	LOW(xrst_gpios);

	k_sched_unlock();
}

static int ls012_write(const struct device *dev, const uint16_t x, const uint16_t y,
		       const struct display_buffer_descriptor *desc, const void *buf)
{
	const struct ls012_config *cfg = dev->config;
	struct ls012_data *data = dev->data;
	const uint8_t *src = buf;

	if (x + desc->width > WIDTH || y + desc->height > HEIGHT || desc->pitch < desc->width) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	for (uint32_t row = 0; row < desc->height; row++) {
		const uint8_t *p = src + row * desc->pitch * 2U;

		for (uint32_t col = 0; col < desc->width; col++, p += 2) {
			data->fb[fb_index(cfg, x + col, y + row)] = from_rgb565(sys_get_le16(p));
		}
	}
	refresh(data);
	k_mutex_unlock(&data->lock);

	return 0;
}

static int ls012_read(const struct device *dev, const uint16_t x, const uint16_t y,
		      const struct display_buffer_descriptor *desc, void *buf)
{
	const struct ls012_config *cfg = dev->config;
	struct ls012_data *data = dev->data;
	uint8_t *dst = buf;

	if (x + desc->width > WIDTH || y + desc->height > HEIGHT || desc->pitch < desc->width) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	for (uint32_t row = 0; row < desc->height; row++) {
		uint8_t *p = dst + row * desc->pitch * 2U;

		for (uint32_t col = 0; col < desc->width; col++, p += 2) {
			sys_put_le16(to_rgb565(data->fb[fb_index(cfg, x + col, y + row)]), p);
		}
	}
	k_mutex_unlock(&data->lock);

	return 0;
}

static void ls012_get_capabilities(const struct device *dev, struct display_capabilities *caps)
{
	ARG_UNUSED(dev);

	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = WIDTH;
	caps->y_resolution = HEIGHT;
	caps->supported_pixel_formats = PIXEL_FORMAT_RGB_565;
	caps->current_pixel_format = PIXEL_FORMAT_RGB_565;
	caps->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

/* The pixels hold their state by themselves; there is nothing to blank. */
static int ls012_blanking(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

/*
 * FRP and XFRP: two 30 Hz square waves in antiphase that alternate the
 * polarity across the liquid crystal. They have to run for as long as the
 * panel is powered, also when nothing is drawn.
 */
static int polarity_start(LPTIM_HandleTypeDef *h, LPTIM_TypeDef *lptim, uint32_t polarity)
{
	LPTIM_OC_ConfigTypeDef oc = {.Pulse = FRP_PERIOD / 2, .OCPolarity = polarity};

	h->Instance = lptim;
	h->Init.Clock.Source = LPTIM_CLOCKSOURCE_APBCLOCK_LPOSC;
	h->Init.Clock.Prescaler = LPTIM_PRESCALER_DIV1;
	h->Init.Trigger.Source = LPTIM_TRIGSOURCE_SOFTWARE;
	h->Init.Period = FRP_PERIOD;
	h->Init.UpdateMode = LPTIM_UPDATE_IMMEDIATE;
	h->Init.CounterSource = LPTIM_COUNTERSOURCE_INTERNAL;
	h->Init.Input1Source = LPTIM_INPUT1SOURCE_GPIO;
	h->Init.Input2Source = LPTIM_INPUT2SOURCE_GPIO;
	h->Init.RepetitionCounter = 0;

	if (HAL_LPTIM_Init(h) != HAL_OK ||
	    HAL_LPTIM_OC_ConfigChannel(h, &oc, LPTIM_CHANNEL_1) != HAL_OK ||
	    HAL_LPTIM_PWM_Start(h, LPTIM_CHANNEL_1) != HAL_OK) {
		return -EIO;
	}
	return 0;
}

static int ls012_init(const struct device *dev)
{
	const struct ls012_config *cfg = dev->config;
	struct ls012_data *data = dev->data;
	int ret;

	k_mutex_init(&data->lock);

	for (int i = 0; i < ARRAY_SIZE(cfg->ctrl); i++) {
		ret = gpio_pin_configure_dt(&cfg->ctrl[i], GPIO_OUTPUT_LOW);
		if (ret < 0) {
			return ret;
		}
	}
	for (int i = 0; i < ARRAY_SIZE(cfg->rgb); i++) {
		ret = gpio_pin_configure_dt(&cfg->rgb[i], GPIO_OUTPUT_LOW);
		if (ret < 0) {
			return ret;
		}
	}

	LL_RCC_SetLPTIMClockSource(LL_RCC_LPTIM1_CLKSOURCE_LSE);
	LL_RCC_SetLPTIMClockSource(LL_RCC_LPTIM34_CLKSOURCE_LSE);
	LL_APB3_GRP1_EnableClock(LL_APB3_GRP1_PERIPH_LPTIM1 | LL_APB3_GRP1_PERIPH_LPTIM3);
	/* Keep both clocked in Stop 2: the polarity signals must never pause. */
	LL_SRDAMR_GRP1_EnableAutonomousClock(LL_SRDAMR_GRP1_PERIPH_LPTIM1AMEN |
					     LL_SRDAMR_GRP1_PERIPH_LPTIM3AMEN);

	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

	ret = polarity_start(&data->frp, LPTIM1, LPTIM_OCPOLARITY_HIGH);
	if (ret == 0) {
		ret = polarity_start(&data->xfrp, LPTIM3, LPTIM_OCPOLARITY_LOW);
	}
	if (ret < 0) {
		LOG_ERR("FRP/XFRP timers did not start");
		return ret;
	}

	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	/* Clear whatever the bootloader left on the panel. */
	refresh(data);

	return 0;
}

static DEVICE_API(display, ls012_api) = {
	.blanking_on = ls012_blanking,
	.blanking_off = ls012_blanking,
	.write = ls012_write,
	.read = ls012_read,
	.get_capabilities = ls012_get_capabilities,
};

PINCTRL_DT_INST_DEFINE(0);

static struct ls012_data ls012_data_0;

static const struct ls012_config ls012_config_0 = {
	.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(0),
	.ctrl = {
		GPIO_DT_SPEC_INST_GET(0, vst_gpios),  GPIO_DT_SPEC_INST_GET(0, vck_gpios),
		GPIO_DT_SPEC_INST_GET(0, enb_gpios),  GPIO_DT_SPEC_INST_GET(0, xrst_gpios),
		GPIO_DT_SPEC_INST_GET(0, hst_gpios),  GPIO_DT_SPEC_INST_GET(0, hck_gpios),
	},
	.rgb = {
		GPIO_DT_SPEC_INST_GET_BY_IDX(0, rgb_gpios, 0),
		GPIO_DT_SPEC_INST_GET_BY_IDX(0, rgb_gpios, 1),
		GPIO_DT_SPEC_INST_GET_BY_IDX(0, rgb_gpios, 2),
		GPIO_DT_SPEC_INST_GET_BY_IDX(0, rgb_gpios, 3),
		GPIO_DT_SPEC_INST_GET_BY_IDX(0, rgb_gpios, 4),
		GPIO_DT_SPEC_INST_GET_BY_IDX(0, rgb_gpios, 5),
	},
	.rotated = DT_INST_PROP(0, rotated),
};

DEVICE_DT_INST_DEFINE(0, ls012_init, NULL, &ls012_data_0, &ls012_config_0, POST_KERNEL,
		      CONFIG_DISPLAY_INIT_PRIORITY, &ls012_api);
