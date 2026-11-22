/*
 * Bluetooth. The radio chip runs the stack; this tells it what the watch is
 * (src/bluenrg.c carries the commands): one service, the Nordic UART service,
 * a serial line to the phone. What the phone writes goes to phone_rx(),
 * bluetooth_send() notifies it.
 *
 * One thread turns the radio on and off as set, keeps it advertising while
 * no phone is connected, and takes the chip's events.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/hash_function.h>
#include <zephyr/sys/util.h>

#include "bluenrg.h"
#include "bluetooth.h"
#include "phone.h"
#include "ui.h"

LOG_MODULE_REGISTER(bluetooth, LOG_LEVEL_INF);

#define NAME "UNA Watch"

/* HCI's own commands */
#define LE_SET_SCAN_RESPONSE BLUENRG_OP(0x08, 0x009)
/* ST's */
#define HAL_FIRMWARE_DETAILS BLUENRG_ACI(0x001)
#define HAL_WRITE_CONFIG     BLUENRG_ACI(0x00c)
#define HAL_SET_TX_POWER     BLUENRG_ACI(0x00f)
#define GAP_SET_DISCOVERABLE BLUENRG_ACI(0x083)
#define GAP_INIT             BLUENRG_ACI(0x08a)
#define GATT_INIT            BLUENRG_ACI(0x101)
#define GATT_ADD_SERVICE     BLUENRG_ACI(0x102)
#define GATT_ADD_CHAR        BLUENRG_ACI(0x104)
#define GATT_UPDATE_VALUE    BLUENRG_ACI(0x106)
#define L2CAP_UPDATE_REQUEST BLUENRG_ACI(0x181)

#define LE_CONNECTED          0x01
#define LE_UPDATED            0x03
#define LE_CONNECTED_ENHANCED 0x0a
#define VENDOR_INITIALIZED    0x0001
#define L2CAP_UPDATE_ANSWERED 0x0800
#define GATT_MODIFIED         0x0c01
#define ATT_MTU_EXCHANGED     0x0c03

#define STATUS_NO_BUFFERS 0x64

/* The static random address to go by, in place of the one the chip keeps. */
#define CONFIG_STATIC_ADDRESS 0x2e

#define UUID_128        0x02
#define PRIMARY         0x01
#define CAN_NOTIFY      0x10
#define CAN_WRITE       0x0c /* with and without response */
#define TELL_WRITES     0x01
#define OWN_RANDOM      0x01
#define ROLE_PERIPHERAL 0x01

/* 6e40000x-b5a3-f393-e0a9-e50e24dcca9e, as it goes over the air */
#define NUS_UUID(x)                                                                                \
	0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, (x), 0x00, 0x40,   \
		0x6e

/* The longest value either way: what Gadgetbridge's MTU of 131 carries. */
#define NUS_MAX 128

/* A second between advertisements: found in a few seconds, at little cost. */
#define ADVERTISE_MIN 0x0640
#define ADVERTISE_MAX 0x0780

/*
 * A phone connects at a fast pace. Once it has said what it had to say the
 * watch asks for a slow one, which is what the battery pays for all day:
 * 100 to 200 ms, of which four in five may be skipped, lost after 6 s.
 */
#define SLOW_AFTER_MS     15000
#define SLOW_INTERVAL_MIN 80
#define SLOW_INTERVAL_MAX 160
#define SLOW_LATENCY      4
#define SLOW_TIMEOUT      600

static uint8_t set_on = 1;
static bool tried;

static struct k_spinlock lock;
static enum bluetooth_state state = BLUETOOTH_OFF;
static bool connected, listening, slow;
static uint16_t conn, interval, latency, mtu;
static uint8_t peer[6], own[6];
static int64_t connected_at_ms;
static uint32_t connects, rx_bytes, tx_bytes;

static uint16_t nus, nus_tx, nus_rx;

static void set_state(enum bluetooth_state new_state)
{
	K_SPINLOCK(&lock) {
		state = new_state;
	}
	ui_refresh();
}

/* A command that has to work; says so in the log when it does not. */
static bool cmd(const char *what, uint16_t opcode, const void *params, size_t len, void *rsp,
		size_t rsp_size)
{
	int ret = bluenrg_cmd(opcode, params, len, rsp, rsp_size);

	if (ret != 0) {
		LOG_ERR("%s: %s 0x%02x", what, ret < 0 ? "error" : "status", ret < 0 ? -ret : ret);
	}
	return ret == 0;
}

static bool add_characteristic(uint8_t which, uint8_t properties, uint8_t tell, uint16_t *handle)
{
	uint8_t p[] = {
		0, 0, UUID_128, NUS_UUID(which), NUS_MAX, 0, properties,
		0,  /* open to a phone that has not paired */
		tell,
		16, /* key size, were it encrypted */
		1,  /* the value's length varies */
	};
	uint8_t rsp[2];

	sys_put_le16(nus, p);
	if (!cmd("add characteristic", GATT_ADD_CHAR, p, sizeof(p), rsp, sizeof(rsp))) {
		return false;
	}
	*handle = sys_get_le16(rsp);
	return true;
}

/*
 * The chip has an address of its own, and the phone app of the stock
 * firmware knows it: it would connect to the watch whenever it saw it, and
 * keep everything else out. Under Zephyr the watch goes by another, made
 * from the MCU's serial number.
 */
static bool set_address(void)
{
	uint8_t p[2 + sizeof(own)] = {CONFIG_STATIC_ADDRESS, sizeof(own)};
	uint8_t id[12];
	uint32_t hash;

	if (hwinfo_get_device_id(id, sizeof(id)) <= 0) {
		return false;
	}
	hash = sys_hash32(id, sizeof(id));
	sys_put_le32(hash, &own[0]);
	sys_put_le16(sys_hash32(&hash, sizeof(hash)), &own[4]);
	own[5] |= 0xc0; /* what marks an address as static random */
	memcpy(&p[2], own, sizeof(own));
	return cmd("address", HAL_WRITE_CONFIG, p, sizeof(p), NULL, 0);
}

static bool turn_on(void)
{
	static const uint8_t power[] = {1, 7}; /* +8 dBm, as the stock firmware */
	static const uint8_t gap[] = {ROLE_PERIPHERAL, 0, sizeof(NAME) - 1};
	static const uint8_t service[] = {
		UUID_128, NUS_UUID(0x01), PRIMARY,
		7, /* attributes: the service, three for one characteristic, two the other */
	};
	uint8_t name[6 + sizeof(NAME) - 1];
	uint8_t rsp[16];
	int ret;

	set_state(BLUETOOTH_STARTING);
	ret = bluenrg_start();
	if (ret != 0) {
		LOG_ERR("the chip did not start: %d", ret);
		return false;
	}
	memset(rsp, 0, sizeof(rsp));
	if (cmd("firmware", HAL_FIRMWARE_DETAILS, NULL, 0, rsp, sizeof(rsp))) {
		LOG_INF("chip firmware %u.%u.%u, stack %u.%u%c", rsp[0], rsp[1], rsp[2], rsp[6],
			rsp[7], 'a' + rsp[8] - 1);
	}
	if (!set_address() || !cmd("power", HAL_SET_TX_POWER, power, sizeof(power), NULL, 0) ||
	    !cmd("GATT", GATT_INIT, NULL, 0, NULL, 0) ||
	    !cmd("GAP", GAP_INIT, gap, sizeof(gap), rsp, 6)) {
		return false;
	}
	/* The name, in the service the chip made for it. */
	memcpy(name, rsp, 4);
	name[4] = 0;
	name[5] = sizeof(NAME) - 1;
	memcpy(&name[6], NAME, sizeof(NAME) - 1);
	if (!cmd("name", GATT_UPDATE_VALUE, name, sizeof(name), NULL, 0) ||
	    !cmd("add service", GATT_ADD_SERVICE, service, sizeof(service), rsp, 2)) {
		return false;
	}
	nus = sys_get_le16(rsp);
	if (!add_characteristic(0x03, CAN_NOTIFY, 0, &nus_tx) ||
	    !add_characteristic(0x02, CAN_WRITE, TELL_WRITES, &nus_rx)) {
		return false;
	}
	LOG_INF("Bluetooth ready, address %02X:%02X:%02X:%02X:%02X:%02X", own[5], own[4], own[3],
		own[2], own[1], own[0]);
	return true;
}

static void turn_off(void)
{
	bool was_connected;

	bluenrg_stop();
	K_SPINLOCK(&lock) {
		was_connected = connected;
		connected = false;
		listening = false;
	}
	if (was_connected) {
		phone_disconnected();
	}
	LOG_INF("Bluetooth off, chip held in reset");
}

static void advertise(void)
{
	/* The name in the advertisement, the service in what a scan is answered. */
	static const uint8_t scan_response[1 + 31] = {18, 17, 0x07, NUS_UUID(0x01)};
	uint8_t p[32];
	size_t n = 0;

	if (!cmd("scan response", LE_SET_SCAN_RESPONSE, scan_response, sizeof(scan_response), NULL,
		 0)) {
		return;
	}
	p[n++] = 0x00; /* connectable, to anyone */
	sys_put_le16(ADVERTISE_MIN, &p[n]);
	sys_put_le16(ADVERTISE_MAX, &p[n + 2]);
	n += 4;
	p[n++] = OWN_RANDOM;
	p[n++] = 0; /* no filter */
	p[n++] = sizeof(NAME);
	p[n++] = 0x09; /* complete name */
	memcpy(&p[n], NAME, sizeof(NAME) - 1);
	n += sizeof(NAME) - 1;
	p[n++] = 0;          /* no services here */
	memset(&p[n], 0, 4); /* no connection interval asked for */
	n += 4;
	if (cmd("advertise", GAP_SET_DISCOVERABLE, p, n, NULL, 0)) {
		LOG_INF("advertising");
		set_state(BLUETOOTH_ADVERTISING);
	}
}

static void slow_down(void)
{
	uint8_t p[10];

	sys_put_le16(conn, &p[0]);
	sys_put_le16(SLOW_INTERVAL_MIN, &p[2]);
	sys_put_le16(SLOW_INTERVAL_MAX, &p[4]);
	sys_put_le16(SLOW_LATENCY, &p[6]);
	sys_put_le16(SLOW_TIMEOUT, &p[8]);
	cmd("slow pace", L2CAP_UPDATE_REQUEST, p, sizeof(p), NULL, 0);
	slow = true;
}

static void on_connected(const uint8_t *d, size_t len, bool enhanced)
{
	const uint8_t *pace = &d[enhanced ? 23 : 11];

	if (len < (enhanced ? 29 : 17) || d[0] != 0) {
		LOG_WRN("connection failed: 0x%02x", d[0]);
		advertise();
		return;
	}
	K_SPINLOCK(&lock) {
		conn = sys_get_le16(&d[1]);
		memcpy(peer, &d[5], sizeof(peer));
		interval = sys_get_le16(&pace[0]);
		latency = sys_get_le16(&pace[2]);
		mtu = 23;
		connected = true;
		listening = false;
		slow = false;
		connected_at_ms = k_uptime_get();
		connects++;
	}
	LOG_INF("connected: %02X:%02X:%02X:%02X:%02X:%02X, interval %u ms", peer[5], peer[4],
		peer[3], peer[2], peer[1], peer[0], interval * 5 / 4);
	set_state(BLUETOOTH_CONNECTED);
}

static void on_disconnected(const uint8_t *d, size_t len)
{
	if (len < 4 || !connected) {
		return;
	}
	LOG_INF("disconnected: 0x%02x", d[3]);
	K_SPINLOCK(&lock) {
		connected = false;
		listening = false;
	}
	phone_disconnected();
	/* The chip needs a moment before it takes the next command. */
	k_msleep(100);
	advertise();
}

/* The phone wrote: to the serial line, or to the switch for our notifications. */
static void on_modified(const uint8_t *d, size_t len)
{
	uint16_t handle, n;

	if (len < 8) {
		return;
	}
	handle = sys_get_le16(&d[2]);
	n = MIN(sys_get_le16(&d[6]), len - 8);
	if (handle == nus_rx + 1) {
		rx_bytes += n;
		phone_rx(&d[8], n);
	} else if (handle == nus_tx + 2 && n >= 1) {
		listening = (d[8] & 1) != 0;
		LOG_INF("serial line %s", listening ? "open" : "closed");
		if (listening) {
			phone_connected();
		}
		ui_refresh();
	}
}

static void on_event(const struct bluenrg_event *ev)
{
	const uint8_t *d = ev->data;

	if (ev->event == BLUENRG_EVENT_DISCONNECTED) {
		on_disconnected(d, ev->len);
	} else if (ev->event == BLUENRG_EVENT_LE && ev->code == LE_CONNECTED) {
		on_connected(d, ev->len, false);
	} else if (ev->event == BLUENRG_EVENT_LE && ev->code == LE_CONNECTED_ENHANCED) {
		on_connected(d, ev->len, true);
	} else if (ev->event == BLUENRG_EVENT_LE && ev->code == LE_UPDATED && ev->len >= 9) {
		interval = sys_get_le16(&d[3]);
		latency = sys_get_le16(&d[5]);
		LOG_INF("interval %u ms, latency %u, timeout %u ms", interval * 5 / 4, latency,
			sys_get_le16(&d[7]) * 10);
		ui_refresh();
	} else if (ev->event == BLUENRG_EVENT_VENDOR && ev->code == GATT_MODIFIED) {
		on_modified(d, ev->len);
	} else if (ev->event == BLUENRG_EVENT_VENDOR && ev->code == ATT_MTU_EXCHANGED &&
		   ev->len >= 4) {
		mtu = sys_get_le16(&d[2]);
		LOG_INF("mtu %u", mtu);
	} else if (ev->event == BLUENRG_EVENT_VENDOR && ev->code == VENDOR_INITIALIZED) {
		/* It restarted by itself and has forgotten everything. */
		LOG_ERR("the chip restarted");
		turn_off();
		set_state(BLUETOOTH_OFF);
	} else if (ev->event == BLUENRG_EVENT_VENDOR && ev->code == L2CAP_UPDATE_ANSWERED &&
		   ev->len >= 4) {
		/* A phone that says no keeps its own pace. */
		LOG_INF("slow pace %s", sys_get_le16(&d[2]) == 0 ? "accepted" : "refused");
	} else if (ev->event != BLUENRG_EVENT_WAKE) {
		LOG_DBG("event 0x%02x 0x%04x, %u bytes", ev->event, ev->code, ev->len);
	}
}

bool bluetooth_send(const void *data, size_t len)
{
	const uint8_t *bytes = data;
	uint8_t p[6 + NUS_MAX];

	while (len > 0) {
		/* Three bytes of each packet are the notification's header. */
		size_t n = MIN(len, MIN(mtu - 3, NUS_MAX));
		int ret = -ENOTCONN;

		sys_put_le16(nus, &p[0]);
		sys_put_le16(nus_tx, &p[2]);
		p[4] = 0;
		p[5] = n;
		memcpy(&p[6], bytes, n);
		/* The chip's buffers are few; they free as the phone takes them. */
		for (int attempt = 0; attempt < 50 && connected && listening; attempt++) {
			ret = bluenrg_cmd(GATT_UPDATE_VALUE, p, 6 + n, NULL, 0);
			if (ret != STATUS_NO_BUFFERS) {
				break;
			}
			k_msleep(10);
		}
		if (ret != 0) {
			return false;
		}
		tx_bytes += n;
		bytes += n;
		len -= n;
	}
	return true;
}

static void address_string(char *str, size_t size, const uint8_t *address)
{
	snprintf(str, size, "%02X:%02X:%02X:%02X:%02X:%02X", address[5], address[4], address[3],
		 address[2], address[1], address[0]);
}

void bluetooth_status(struct bluetooth_status *status)
{
	memset(status, 0, sizeof(*status));
	K_SPINLOCK(&lock) {
		status->state = state;
		status->listening = listening;
		status->connects = connects;
		status->rx_bytes = rx_bytes;
		status->tx_bytes = tx_bytes;
	}
	if (status->state == BLUETOOTH_ADVERTISING || status->state == BLUETOOTH_CONNECTED) {
		address_string(status->own, sizeof(status->own), own);
	}
	if (status->state == BLUETOOTH_CONNECTED) {
		address_string(status->peer, sizeof(status->peer), peer);
		status->interval_ms = interval * 5 / 4;
		status->latency = latency;
		status->mtu = mtu;
		status->connected_s = (k_uptime_get() - connected_at_ms) / 1000;
	}
}

bool bluetooth_is_set(void)
{
	return set_on;
}

void bluetooth_set(bool on)
{
	set_on = on;
	tried = false;
	settings_save_one("radio/on", &set_on, sizeof(set_on));
	bluenrg_wake();
}

static int bluetooth_setting_set(const char *name, size_t len, settings_read_cb read_cb,
				 void *cb_arg)
{
	if (settings_name_steq(name, "on", NULL) && len == sizeof(set_on)) {
		return read_cb(cb_arg, &set_on, sizeof(set_on)) < 0 ? -EIO : 0;
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(radio, "radio", NULL, bluetooth_setting_set, NULL, NULL);

static void bluetooth_thread(void *p1, void *p2, void *p3)
{
	static struct bluenrg_event ev;
	bool on = false;

	bluenrg_stop();
	while (true) {
		k_timeout_t wait = K_FOREVER;

		if (on && state == BLUETOOTH_OFF) {
			/* The chip restarted: start over. */
			on = false;
			tried = false;
		}
		if (set_on && !on && !tried) {
			tried = true;
			on = turn_on();
			if (on) {
				advertise();
			} else {
				bluenrg_stop();
				set_state(BLUETOOTH_FAILED);
			}
		} else if (!set_on && (on || state != BLUETOOTH_OFF)) {
			turn_off();
			on = false;
			set_state(BLUETOOTH_OFF);
		}
		if (connected && !slow) {
			int64_t left = connected_at_ms + SLOW_AFTER_MS - k_uptime_get();

			if (left <= 0) {
				slow_down();
				continue;
			}
			wait = K_MSEC(left);
		}
		if (bluenrg_event(&ev, wait)) {
			on_event(&ev);
		}
	}
}
K_THREAD_DEFINE(bluetooth_tid, 4096, bluetooth_thread, NULL, NULL, NULL, 7, 0, 3000);

/* radio [on|off] */
static int cmd_radio(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const names[] = {"off", "starting", "advertising", "connected",
					    "failed"};
	struct bluetooth_status s;

	if (argc > 1) {
		bluetooth_set(strcmp(argv[1], "on") == 0);
	}
	bluetooth_status(&s);
	shell_print(sh, "%s (set %s), own %s, peer %s%s", names[s.state],
		    bluetooth_is_set() ? "on" : "off", s.own[0] ? s.own : "-",
		    s.peer[0] ? s.peer : "none", s.listening ? ", serial line open" : "");
	shell_print(sh,
		    "interval %u ms, latency %u, mtu %u, connected %u s, %u connects, "
		    "rx %u tx %u bytes",
		    s.interval_ms, s.latency, s.mtu, s.connected_s, s.connects, s.rx_bytes,
		    s.tx_bytes);
	return 0;
}
SHELL_CMD_ARG_REGISTER(radio, NULL, "Bluetooth state, or turn it on or off: radio [on|off]",
		       cmd_radio, 1, 1);
