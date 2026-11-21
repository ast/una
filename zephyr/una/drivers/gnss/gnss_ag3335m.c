/*
 * Airoha AG3335M GNSS receiver.
 *
 * After Zephyr's driver for Quectel's LC76G, which is this chip in a
 * module: the same NMEA matching and PAIR commands. What differs is what
 * the watch needs: the chip's power is a pin, it is off until asked for,
 * and its sleep is locked before anything else, as the stock firmware does
 * (GpsAiroha.cpp in una-kernel-public).
 */

#define DT_DRV_COMPAT airoha_ag3335m

#include <string.h>

#include <zephyr/drivers/gnss.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/modem/backend/uart.h>
#include <zephyr/modem/chat.h>

#include <una/gnss_ag3335m.h>

#include "gnss_nmea0183.h"
#include "gnss_nmea0183_match.h"

LOG_MODULE_REGISTER(gnss_ag3335m, CONFIG_GNSS_LOG_LEVEL);

/* From power to the first command, as the stock firmware waits. */
#define BOOT_MS 300
#define ACK_TIMEOUT_S 2

struct ag3335m_config {
	const struct device *uart;
	struct gpio_dt_spec enable;
};

struct ag3335m_data {
	struct gnss_nmea0183_match_data match_data;
#if CONFIG_GNSS_SATELLITES
	struct gnss_satellite satellites[CONFIG_GNSS_AG3335M_SAT_ARRAY_SIZE];
#endif

	struct modem_pipe *uart_pipe;
	struct modem_backend_uart uart_backend;
	uint8_t uart_backend_receive_buf[2048];
	uint8_t uart_backend_transmit_buf[64];

	struct modem_chat chat;
	uint8_t chat_receive_buf[256];
	uint8_t chat_delimiter[2];
	uint8_t *chat_argv[32];

	/* One command and the acknowledgement awaited for it. */
	uint8_t pair_request_buf[32];
	uint8_t pair_match_buf[32];
	struct modem_chat_match pair_match;
	struct modem_chat_script_chat pair_script_chat;
	struct modem_chat_script pair_script;

	struct k_sem lock;
	bool on;
	uint32_t sentences;
};

static void on_gga(struct modem_chat *chat, char **argv, uint16_t argc, void *user_data)
{
	struct ag3335m_data *data = CONTAINER_OF(chat, struct ag3335m_data, chat);

	data->sentences++;
	gnss_nmea0183_match_gga_callback(chat, argv, argc, user_data);
}

static void on_rmc(struct modem_chat *chat, char **argv, uint16_t argc, void *user_data)
{
	struct ag3335m_data *data = CONTAINER_OF(chat, struct ag3335m_data, chat);

	data->sentences++;
	gnss_nmea0183_match_rmc_callback(chat, argv, argc, user_data);
}

#if CONFIG_GNSS_SATELLITES
static void on_gsv(struct modem_chat *chat, char **argv, uint16_t argc, void *user_data)
{
	struct ag3335m_data *data = CONTAINER_OF(chat, struct ag3335m_data, chat);

	data->sentences++;
	gnss_nmea0183_match_gsv_callback(chat, argv, argc, user_data);
}
#endif

MODEM_CHAT_MATCHES_DEFINE(unsol_matches,
	MODEM_CHAT_MATCH_WILDCARD("$??GGA,", ",*", on_gga),
	MODEM_CHAT_MATCH_WILDCARD("$??RMC,", ",*", on_rmc),
#if CONFIG_GNSS_SATELLITES
	MODEM_CHAT_MATCH_WILDCARD("$??GSV,", ",*", on_gsv),
#endif
);

/* Send "PAIRnnn[,...]" and wait for "PAIR001,nnn,0", its acknowledgement. */
static int pair(const struct device *dev, const char *command)
{
	struct ag3335m_data *data = dev->data;
	int ret;

	ret = gnss_nmea0183_snprintk(data->pair_request_buf, sizeof(data->pair_request_buf), "%s",
				     command);
	if (ret < 0) {
		return ret;
	}
	ret = modem_chat_script_chat_set_request(&data->pair_script_chat, data->pair_request_buf);
	if (ret < 0) {
		return ret;
	}
	ret = gnss_nmea0183_snprintk(data->pair_match_buf, sizeof(data->pair_match_buf),
				     "PAIR001,%.3s,0", command + strlen("PAIR"));
	if (ret < 0) {
		return ret;
	}
	ret = modem_chat_match_set_match(&data->pair_match, data->pair_match_buf);
	if (ret < 0) {
		return ret;
	}
	ret = modem_chat_run_script(&data->chat, &data->pair_script);
	if (ret < 0) {
		LOG_WRN("%s not acknowledged: %d", command, ret);
	}
	return ret;
}

static int turn_on(const struct device *dev)
{
	/*
	 * Sleep locked, as stock does first: without it the chip stops
	 * answering after a command or two. Then the receiver started, which
	 * sentences it sends (GGA and RMC every fix, the satellites every
	 * fifth, the others never), and a hot start.
	 *
	 * Stock also sends PAIR003 (receiver off) and PAIR104,0 (L1 only)
	 * before starting it; this chip answers neither and goes on as it was.
	 */
	static const char *const commands[] = {
		"PAIR382,1",   "PAIR002",     "PAIR062,0,1", "PAIR062,1,0", "PAIR062,2,0",
#if CONFIG_GNSS_SATELLITES
		"PAIR062,3,5",
#else
		"PAIR062,3,0",
#endif
		"PAIR062,4,1", "PAIR062,5,0", "PAIR004",
	};
	const struct ag3335m_config *config = dev->config;
	struct ag3335m_data *data = dev->data;
	int acknowledged = 0;
	int ret;

	gpio_pin_set_dt(&config->enable, 1);
	k_sleep(K_MSEC(BOOT_MS));

	ret = modem_pipe_open(data->uart_pipe, K_SECONDS(10));
	if (ret < 0) {
		LOG_ERR("pipe not opened: %d", ret);
		goto fail;
	}
	ret = modem_chat_attach(&data->chat, data->uart_pipe);
	if (ret < 0) {
		LOG_ERR("chat not attached: %d", ret);
		modem_pipe_close(data->uart_pipe, K_SECONDS(10));
		goto fail;
	}
	data->sentences = 0;
	for (int i = 0; i < ARRAY_SIZE(commands); i++) {
		acknowledged += pair(dev, commands[i]) == 0;
	}
	/* One that answers nothing is not there; one that answers some may still work. */
	if (acknowledged == 0) {
		ret = -EIO;
		modem_pipe_close(data->uart_pipe, K_SECONDS(10));
		goto fail;
	}
	LOG_INF("on: %d of %d commands acknowledged", acknowledged, (int)ARRAY_SIZE(commands));
	return 0;

fail:
	gpio_pin_set_dt(&config->enable, 0);
	return ret;
}

static void turn_off(const struct device *dev)
{
	const struct ag3335m_config *config = dev->config;
	struct ag3335m_data *data = dev->data;

	modem_pipe_close(data->uart_pipe, K_SECONDS(10));
	gpio_pin_set_dt(&config->enable, 0);
	LOG_INF("off after %u sentences", data->sentences);
}

int gnss_ag3335m_power(const struct device *dev, bool on)
{
	struct ag3335m_data *data = dev->data;
	int ret = 0;

	(void)k_sem_take(&data->lock, K_FOREVER);
	if (on && !data->on) {
		ret = turn_on(dev);
		data->on = ret == 0;
	} else if (!on && data->on) {
		turn_off(dev);
		data->on = false;
	}
	k_sem_give(&data->lock);
	return ret;
}

uint32_t gnss_ag3335m_sentences(const struct device *dev)
{
	struct ag3335m_data *data = dev->data;

	return data->sentences;
}

/* Fix rate, navigation mode and systems are left as the chip has them. */
static DEVICE_API(gnss, gnss_api) = {};

static int ag3335m_init(const struct device *dev)
{
	const struct ag3335m_config *config = dev->config;
	struct ag3335m_data *data = dev->data;
	int ret;

	const struct gnss_nmea0183_match_config match_config = {
		.gnss = dev,
#if CONFIG_GNSS_SATELLITES
		.satellites = data->satellites,
		.satellites_size = ARRAY_SIZE(data->satellites),
#endif
	};
	const struct modem_backend_uart_config uart_backend_config = {
		.uart = config->uart,
		.receive_buf = data->uart_backend_receive_buf,
		.receive_buf_size = ARRAY_SIZE(data->uart_backend_receive_buf),
		.transmit_buf = data->uart_backend_transmit_buf,
		.transmit_buf_size = ARRAY_SIZE(data->uart_backend_transmit_buf),
	};
	const struct modem_chat_config chat_config = {
		.user_data = data,
		.receive_buf = data->chat_receive_buf,
		.receive_buf_size = ARRAY_SIZE(data->chat_receive_buf),
		.delimiter = data->chat_delimiter,
		.delimiter_size = ARRAY_SIZE(data->chat_delimiter),
		.argv = data->chat_argv,
		.argv_size = ARRAY_SIZE(data->chat_argv),
		.unsol_matches = unsol_matches,
		.unsol_matches_size = ARRAY_SIZE(unsol_matches),
	};

	k_sem_init(&data->lock, 1, 1);

	if (!gpio_is_ready_dt(&config->enable)) {
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&config->enable, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}
	ret = gnss_nmea0183_match_init(&data->match_data, &match_config);
	if (ret < 0) {
		return ret;
	}
	data->uart_pipe = modem_backend_uart_init(&data->uart_backend, &uart_backend_config);
	ret = modem_chat_init(&data->chat, &chat_config);
	if (ret < 0) {
		return ret;
	}

	modem_chat_match_init(&data->pair_match);
	modem_chat_match_set_separators(&data->pair_match, ",*");
	modem_chat_script_chat_init(&data->pair_script_chat);
	modem_chat_script_chat_set_response_matches(&data->pair_script_chat, &data->pair_match, 1);
	modem_chat_script_init(&data->pair_script);
	modem_chat_script_set_name(&data->pair_script, "pair");
	modem_chat_script_set_script_chats(&data->pair_script, &data->pair_script_chat, 1);
	modem_chat_script_set_abort_matches(&data->pair_script, NULL, 0);
	modem_chat_script_set_timeout(&data->pair_script, ACK_TIMEOUT_S);
	return 0;
}

#define AG3335M_DEVICE(inst)                                                                       \
	static const struct ag3335m_config ag3335m_config_##inst = {                               \
		.uart = DEVICE_DT_GET(DT_INST_BUS(inst)),                                          \
		.enable = GPIO_DT_SPEC_INST_GET(inst, enable_gpios),                               \
	};                                                                                         \
                                                                                                   \
	static struct ag3335m_data ag3335m_data_##inst = {                                         \
		.chat_delimiter = {'\r', '\n'},                                                    \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, ag3335m_init, NULL, &ag3335m_data_##inst,                      \
			      &ag3335m_config_##inst, POST_KERNEL, CONFIG_GNSS_INIT_PRIORITY,      \
			      &gnss_api);

DT_INST_FOREACH_STATUS_OKAY(AG3335M_DEVICE)
