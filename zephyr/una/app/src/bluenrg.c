/*
 * BlueNRG-2N over SPI.
 *
 * The chip raises its IRQ line when it has something to be read. Every
 * transfer starts with five bytes each way: ours say read or write, the
 * chip's how many bytes it has for us and how many it can take. A sleeping
 * chip wakes on chip select and raises IRQ when it is ready for the five.
 *
 * One thread reads. An answer to a command goes to whoever waits in
 * bluenrg_cmd(), everything else into a queue for bluenrg_event().
 */

#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "bluenrg.h"

LOG_MODULE_REGISTER(bluenrg, LOG_LEVEL_INF);

#define NODE DT_NODELABEL(hci_spi)

static const struct spi_dt_spec bus =
	SPI_DT_SPEC_GET(NODE, SPI_OP_MODE_CONTROLLER | SPI_TRANSFER_MSB | SPI_WORD_SET(8) |
				      SPI_LOCK_ON);
static const struct gpio_dt_spec irq = GPIO_DT_SPEC_GET(NODE, irq_gpios);
static const struct gpio_dt_spec reset = GPIO_DT_SPEC_GET(NODE, reset_gpios);

#define SPI_WRITE 0x0a
#define SPI_READ  0x0b

#define PACKET_COMMAND   0x01
#define PACKET_EVENT     0x04
#define PACKET_EVENT_EXT 0x82 /* ST's: an event with a two-byte length */

#define EVENT_COMMAND_COMPLETE 0x0e
#define EVENT_COMMAND_STATUS   0x0f
#define VENDOR_INITIALIZED     0x0001

#define RESET_MS   10
#define START_MS   2000
#define ANSWER_MS  2000
#define PACKET_MAX 520

static K_MUTEX_DEFINE(bus_lock);
static K_MUTEX_DEFINE(cmd_lock);
static K_SEM_DEFINE(irq_raised, 0, 1);
static K_SEM_DEFINE(started, 0, 1);
static K_SEM_DEFINE(answered, 0, 1);
K_MSGQ_DEFINE(bluenrg_events, sizeof(struct bluenrg_event), 6, 4);

static struct gpio_callback irq_callback;
static bool running, starting;

/* The command being waited for, and its answer. */
static struct k_spinlock pending_lock;
static uint16_t pending_opcode;
static bool pending;
static uint8_t answer[PACKET_MAX];
static size_t answer_len;

static void on_irq(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_sem_give(&irq_raised);
}

static int transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
	const struct spi_buf tx_buf = {.buf = (void *)tx, .len = len};
	const struct spi_buf rx_buf = {.buf = rx, .len = len};
	const struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};
	const struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1};

	return spi_transceive_dt(&bus, tx != NULL ? &tx_set : NULL, rx != NULL ? &rx_set : NULL);
}

/*
 * Select the chip, wait until it is ready and exchange the five bytes.
 * The bus stays ours, with the chip selected, until deselect().
 */
static int select(uint8_t op, uint16_t *to_read, uint16_t *to_write)
{
	uint8_t ours[5] = {op, 0, 0, 0, 0};
	uint8_t theirs[5];
	int ret;

	/* Nothing goes over the bus: this takes it and lowers chip select. */
	ret = transfer(ours, NULL, 0);
	if (ret != 0) {
		return ret;
	}
	if (!WAIT_FOR(gpio_pin_get_dt(&irq) > 0, 100000, k_usleep(100))) {
		return -ETIMEDOUT;
	}
	ret = transfer(ours, theirs, sizeof(ours));
	*to_write = sys_get_le16(&theirs[1]);
	*to_read = sys_get_le16(&theirs[3]);
	return ret;
}

static void deselect(bool wait_low)
{
	spi_release_dt(&bus);
	if (wait_low) {
		/* The chip lowers IRQ once it has seen the end of the transfer. */
		WAIT_FOR(gpio_pin_get_dt(&irq) == 0, 10000, k_usleep(50));
	}
}

/* One packet from the chip, if it has one. Returns its length. */
static int read_packet(uint8_t *packet, size_t size)
{
	uint16_t to_read = 0, to_write;
	int ret;

	k_mutex_lock(&bus_lock, K_FOREVER);
	if (gpio_pin_get_dt(&irq) <= 0) {
		k_mutex_unlock(&bus_lock);
		return 0;
	}
	ret = select(SPI_READ, &to_read, &to_write);
	if (ret == 0 && to_read > size) {
		LOG_ERR("packet of %u bytes", to_read);
		ret = -EMSGSIZE;
	}
	if (ret == 0 && to_read > 0) {
		/* What goes out meanwhile is all ones, as ST's own code sends. */
		static uint8_t ones[PACKET_MAX];

		memset(ones, 0xff, to_read);
		ret = transfer(ones, packet, to_read);
	}
	deselect(ret == 0 && to_read > 0);
	k_mutex_unlock(&bus_lock);
	return ret < 0 ? ret : to_read;
}

static int write_packet(const uint8_t *packet, size_t len)
{
	uint16_t to_read, to_write = 0;
	int ret;

	k_mutex_lock(&bus_lock, K_FOREVER);
	/* The chip wants a pause after the transfer before. */
	k_usleep(100);
	ret = select(SPI_WRITE, &to_read, &to_write);
	if (ret == 0 && to_write < len) {
		ret = -ENOBUFS;
	}
	if (ret == 0) {
		ret = transfer(packet, NULL, len);
	}
	deselect(ret == 0);
	k_mutex_unlock(&bus_lock);
	return ret;
}

/* An answer is a command-complete or command-status event with our opcode. */
static bool take_answer(const uint8_t *packet, size_t len)
{
	uint16_t opcode;
	bool taken = false;

	if (packet[0] != PACKET_EVENT || len < 6) {
		return false;
	}
	if (packet[1] == EVENT_COMMAND_COMPLETE) {
		opcode = sys_get_le16(&packet[4]);
	} else if (packet[1] == EVENT_COMMAND_STATUS && len >= 7) {
		opcode = sys_get_le16(&packet[5]);
	} else {
		return false;
	}
	K_SPINLOCK(&pending_lock) {
		if (pending && opcode == pending_opcode) {
			if (packet[1] == EVENT_COMMAND_COMPLETE) {
				answer_len = len - 6;
				memcpy(answer, &packet[6], answer_len);
			} else {
				answer_len = 1;
				answer[0] = packet[3];
			}
			pending = false;
			taken = true;
		}
	}
	if (taken) {
		k_sem_give(&answered);
	}
	/* An answer nobody waits for is nothing to anyone else either. */
	return true;
}

static void queue_event(const uint8_t *packet, size_t len)
{
	static struct bluenrg_event ev;
	const uint8_t *body;
	size_t body_len;

	if (packet[0] == PACKET_EVENT && len >= 3) {
		body = &packet[3];
		body_len = MIN(packet[2], len - 3);
	} else if (packet[0] == PACKET_EVENT_EXT && len >= 4) {
		body = &packet[4];
		body_len = MIN(sys_get_le16(&packet[2]), len - 4);
	} else {
		LOG_WRN("packet of type 0x%02x", packet[0]);
		return;
	}
	ev.event = packet[1];
	ev.code = 0;
	if (ev.event == BLUENRG_EVENT_LE && body_len >= 1) {
		ev.code = body[0];
		body += 1;
		body_len -= 1;
	} else if (ev.event == BLUENRG_EVENT_VENDOR && body_len >= 2) {
		ev.code = sys_get_le16(body);
		body += 2;
		body_len -= 2;
		if (ev.code == VENDOR_INITIALIZED && starting) {
			/* As asked for: bluenrg_start() waits for this one. */
			starting = false;
			k_sem_give(&started);
			return;
		}
	}
	ev.len = MIN(body_len, sizeof(ev.data));
	memcpy(ev.data, body, ev.len);
	if (k_msgq_put(&bluenrg_events, &ev, K_MSEC(500)) != 0) {
		LOG_ERR("event 0x%02x 0x%04x lost: nobody takes them", ev.event, ev.code);
	}
}

static void read_thread(void *p1, void *p2, void *p3)
{
	static uint8_t packet[PACKET_MAX];
	int len;

	while (true) {
		k_sem_take(&irq_raised, K_FOREVER);
		while (running && gpio_pin_get_dt(&irq) > 0) {
			len = read_packet(packet, sizeof(packet));
			if (len < 0) {
				LOG_ERR("read: %d", len);
				break;
			}
			if (len >= 2 && !take_answer(packet, len)) {
				queue_event(packet, len);
			}
		}
	}
}
K_THREAD_DEFINE(bluenrg_tid, 2048, read_thread, NULL, NULL, NULL, 6, 0, 0);

int bluenrg_cmd(uint16_t opcode, const void *params, size_t len, void *rsp, size_t rsp_size)
{
	static uint8_t packet[4 + 255];
	int ret;

	if (len > 255) {
		return -EINVAL;
	}
	k_mutex_lock(&cmd_lock, K_FOREVER);
	if (!running) {
		k_mutex_unlock(&cmd_lock);
		return -ENETDOWN;
	}
	packet[0] = PACKET_COMMAND;
	sys_put_le16(opcode, &packet[1]);
	packet[3] = len;
	if (len > 0) {
		memcpy(&packet[4], params, len);
	}
	k_sem_reset(&answered);
	K_SPINLOCK(&pending_lock) {
		pending_opcode = opcode;
		pending = true;
	}
	ret = write_packet(packet, 4 + len);
	if (ret == 0 && k_sem_take(&answered, K_MSEC(ANSWER_MS)) != 0) {
		ret = -ETIMEDOUT;
	}
	K_SPINLOCK(&pending_lock) {
		pending = false;
	}
	if (ret == 0) {
		if (answer_len < 1) {
			ret = -EBADMSG;
		} else {
			ret = answer[0];
			if (rsp != NULL) {
				memcpy(rsp, &answer[1], MIN(rsp_size, answer_len - 1));
			}
		}
	}
	if (ret != 0) {
		LOG_DBG("command 0x%04x: %d", opcode, ret);
	}
	k_mutex_unlock(&cmd_lock);
	return ret;
}

bool bluenrg_event(struct bluenrg_event *event, k_timeout_t timeout)
{
	return k_msgq_get(&bluenrg_events, event, timeout) == 0;
}

void bluenrg_wake(void)
{
	static const struct bluenrg_event wake = {.event = BLUENRG_EVENT_WAKE};

	k_msgq_put(&bluenrg_events, &wake, K_NO_WAIT);
}

int bluenrg_start(void)
{
	static bool set_up;

	if (!set_up) {
		if (!spi_is_ready_dt(&bus) || !gpio_is_ready_dt(&irq) ||
		    !gpio_is_ready_dt(&reset)) {
			return -ENODEV;
		}
		gpio_pin_configure_dt(&irq, GPIO_INPUT);
		gpio_init_callback(&irq_callback, on_irq, BIT(irq.pin));
		gpio_add_callback(irq.port, &irq_callback);
		set_up = true;
	}
	k_mutex_lock(&cmd_lock, K_FOREVER);
	gpio_pin_configure_dt(&reset, GPIO_OUTPUT_ACTIVE);
	k_msleep(RESET_MS);
	k_msgq_purge(&bluenrg_events);
	k_sem_reset(&started);
	running = true;
	starting = true;
	gpio_pin_interrupt_configure_dt(&irq, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_pin_set_dt(&reset, 0);
	k_mutex_unlock(&cmd_lock);

	if (k_sem_take(&started, K_MSEC(START_MS)) != 0) {
		bluenrg_stop();
		return -ETIMEDOUT;
	}
	return 0;
}

void bluenrg_stop(void)
{
	k_mutex_lock(&cmd_lock, K_FOREVER);
	running = false;
	gpio_pin_interrupt_configure_dt(&irq, GPIO_INT_DISABLE);
	gpio_pin_configure_dt(&reset, GPIO_OUTPUT_ACTIVE);
	k_mutex_unlock(&cmd_lock);
}
