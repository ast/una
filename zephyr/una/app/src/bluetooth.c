/*
 * Bluetooth, with Zephyr's own host stack. The radio chip (BlueNRG-2N) is
 * told to be a plain controller, which it does properly from ST's firmware
 * 3.4.0 on: see bnrg-update.conf for how that gets into it.
 *
 * The watch offers one service, the Nordic UART service, a serial line to
 * the phone. What the phone writes goes to phone_rx(), bluetooth_send()
 * notifies it.
 *
 * One thread turns the radio on and off as set, keeps it advertising while
 * no phone is connected, and passes on what the stack's callbacks leave for
 * it: they run on the stack's own threads, where nothing may wait.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/hash_function.h>
#include <zephyr/sys/util.h>

#include "bluetooth.h"
#include "phone.h"
#include "ui.h"

LOG_MODULE_REGISTER(bluetooth, LOG_LEVEL_INF);

/* ST's own commands, which the chip takes as a plain controller too. */
#define HAL_FIRMWARE_DETAILS BT_OP(BT_OGF_VS, 0x001)
#define HAL_SET_TX_POWER     BT_OP(BT_OGF_VS, 0x00f)

/* A second between advertisements: found in a few seconds, at little cost. */
#define ADVERTISE_MIN 0x0640
#define ADVERTISE_MAX 0x0780

/*
 * How well the phone is heard, asked of the chip now and then. It is in the
 * log every minute: a chip that answers still has a link, whatever else.
 */
#define RSSI_FIRST_MS 20000
#define RSSI_EVERY_MS 60000

/* What a callback leaves for the thread to do. */
enum {
	DO_ADVERTISE,
	DO_OPENED, /* the phone listens */
	DO_CLOSED, /* the link is gone */
	DO_RESTART,
	DO_COUNT,
};
static ATOMIC_DEFINE(todo, DO_COUNT);
static K_SEM_DEFINE(wake, 0, 1);

/* What the phone wrote, on its way to the thread. */
struct chunk {
	uint16_t len;
	uint8_t data[CONFIG_BT_L2CAP_TX_MTU - 3];
};
K_MSGQ_DEFINE(received, sizeof(struct chunk), 8, 2);

static uint8_t set_on = 1;
static bool tried;

static struct k_spinlock lock;
static enum bluetooth_state state = BLUETOOTH_OFF;
static struct bt_conn *link;
static bool listening;
static uint16_t interval_ms, latency, mtu;
static bt_addr_le_t peer, own;
static int64_t connected_at_ms, rssi_at_ms;
static uint32_t connects, resets, rx_bytes, tx_bytes;
static int rssi_status;
static int8_t rssi;

static void address_string(char *str, size_t size, const bt_addr_le_t *address)
{
	const uint8_t *a = address->a.val;

	snprintf(str, size, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

static void set_state(enum bluetooth_state to)
{
	K_SPINLOCK(&lock) {
		state = to;
	}
	ui_refresh();
}

static void ask(int what)
{
	atomic_set_bit(todo, what);
	k_sem_give(&wake);
}

/* The link, held on to until bt_conn_unref(); NULL when there is none. */
static struct bt_conn *link_ref(void)
{
	struct bt_conn *conn = NULL;

	K_SPINLOCK(&lock) {
		if (link != NULL) {
			conn = bt_conn_ref(link);
		}
	}
	return conn;
}

void bluetooth_status(struct bluetooth_status *status)
{
	memset(status, 0, sizeof(*status));
	K_SPINLOCK(&lock) {
		status->state = state;
		status->listening = listening;
		status->connects = connects;
		status->resets = resets;
		status->rx_bytes = rx_bytes;
		status->tx_bytes = tx_bytes;
		if (state == BLUETOOTH_ADVERTISING || state == BLUETOOTH_CONNECTED) {
			address_string(status->own, sizeof(status->own), &own);
		}
		if (state == BLUETOOTH_CONNECTED) {
			address_string(status->peer, sizeof(status->peer), &peer);
			status->interval_ms = interval_ms;
			status->latency = latency;
			status->mtu = mtu;
			status->connected_s = (k_uptime_get() - connected_at_ms) / 1000;
			status->rssi = rssi;
			status->rssi_status = rssi_status;
		}
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
	k_sem_give(&wake);
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

bool bluetooth_send(const void *data, size_t len)
{
	struct bt_conn *conn = link_ref();
	const uint8_t *at = data;
	bool sent = conn != NULL && listening;

	while (sent && len > 0) {
		size_t n = MIN(len, bt_gatt_get_mtu(conn) - 3);
		int ret = bt_nus_send(conn, at, n);

		if (ret != 0) {
			LOG_WRN("send: %d", ret);
			sent = false;
			break;
		}
		at += n;
		len -= n;
		K_SPINLOCK(&lock) {
			tx_bytes += n;
		}
	}
	if (conn != NULL) {
		bt_conn_unref(conn);
	}
	return sent;
}

/* The stack's callbacks */

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;
	char str[18];

	if (err != 0) {
		LOG_WRN("a connection failed: 0x%02x", err);
		return;
	}
	K_SPINLOCK(&lock) {
		link = bt_conn_ref(conn);
		listening = false;
		peer = *bt_conn_get_dst(conn);
		mtu = bt_gatt_get_mtu(conn);
		interval_ms = 0;
		latency = 0;
		if (bt_conn_get_info(conn, &info) == 0) {
			interval_ms = info.le.interval_us / 1000;
			latency = info.le.latency;
		}
		connected_at_ms = k_uptime_get();
		rssi_at_ms = connected_at_ms + RSSI_FIRST_MS - RSSI_EVERY_MS;
		rssi = 0;
		rssi_status = 0;
		connects++;
		state = BLUETOOTH_CONNECTED;
	}
	address_string(str, sizeof(str), &peer);
	LOG_INF("connected to %s, interval %u ms", str, interval_ms);
	ui_refresh();
	k_sem_give(&wake);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct bt_conn *was = NULL;

	K_SPINLOCK(&lock) {
		if (conn == link) {
			was = link;
			link = NULL;
			listening = false;
			state = BLUETOOTH_STARTING;
		}
	}
	if (was == NULL) {
		return;
	}
	bt_conn_unref(was);
	LOG_INF("disconnected, reason 0x%02x", reason);
	ask(DO_CLOSED);
}

/* The connection is free to be had again: only now can advertising start. */
static void on_recycled(void)
{
	ask(DO_ADVERTISE);
}

static void on_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t new_latency,
			     uint16_t timeout)
{
	K_SPINLOCK(&lock) {
		interval_ms = interval * 5 / 4;
		latency = new_latency;
	}
	LOG_INF("interval %u ms, latency %u, timeout %u ms", interval * 5 / 4, new_latency,
		timeout * 10);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.recycled = on_recycled,
	.le_param_updated = on_param_updated,
};

static void on_mtu(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	K_SPINLOCK(&lock) {
		mtu = bt_gatt_get_mtu(conn);
	}
}

static struct bt_gatt_cb gatt_callbacks = {.att_mtu_updated = on_mtu};

static void on_listening(bool enabled, void *ctx)
{
	K_SPINLOCK(&lock) {
		listening = enabled && link != NULL;
	}
	if (enabled) {
		ask(DO_OPENED);
	}
	ui_refresh();
}

static void on_received(struct bt_conn *conn, const void *data, uint16_t len, void *ctx)
{
	static struct chunk chunk;

	chunk.len = MIN(len, sizeof(chunk.data));
	memcpy(chunk.data, data, chunk.len);
	K_SPINLOCK(&lock) {
		rx_bytes += len;
	}
	if (k_msgq_put(&received, &chunk, K_MSEC(500)) != 0) {
		LOG_ERR("%u bytes from the phone lost: nobody takes them", len);
	}
	k_sem_give(&wake);
}

static struct bt_nus_cb nus_callbacks = {
	.notif_enabled = on_listening,
	.received = on_received,
};

/* The thread's own */

/*
 * The address to go by: a static random one made of the SoC's serial number,
 * and not the one the chip keeps, by which the phone knows the watch as it
 * was sold. Before the stack starts, so that it has no other.
 */
static int set_address(void)
{
	bt_addr_le_t addr = {.type = BT_ADDR_LE_RANDOM};
	uint8_t id[12];
	uint32_t hash;
	size_t count = 0;
	int ret;

	bt_id_get(NULL, &count);
	if (count > 0) {
		return 0;
	}
	if (hwinfo_get_device_id(id, sizeof(id)) <= 0) {
		return -EIO;
	}
	hash = sys_hash32(id, sizeof(id));
	sys_put_le32(hash, &addr.a.val[0]);
	sys_put_le16(sys_hash32(&hash, sizeof(hash)), &addr.a.val[4]);
	BT_ADDR_SET_STATIC(&addr.a);
	ret = bt_id_create(&addr, NULL);
	return ret < 0 ? ret : 0;
}

static int vendor_cmd(uint16_t opcode, const void *params, size_t len, struct net_buf **rsp)
{
	struct net_buf *buf = bt_hci_cmd_alloc(K_SECONDS(1));

	if (buf == NULL) {
		return -ENOBUFS;
	}
	net_buf_add_mem(buf, params, len);
	return bt_hci_cmd_send_sync(opcode, buf, rsp);
}

/* What the chip runs, for the log; and whether that will do. */
static void check_firmware(void)
{
	struct net_buf *rsp = NULL;
	const uint8_t *d;

	if (vendor_cmd(HAL_FIRMWARE_DETAILS, NULL, 0, &rsp) != 0 || rsp->len < 10) {
		LOG_WRN("the chip does not say what firmware it has");
	} else {
		d = &rsp->data[1];
		LOG_INF("chip firmware %u.%u.%u, stack %u.%u%c", d[0], d[1], d[2], d[6], d[7],
			'a' + d[8] - 1);
		if (d[0] < 3 || (d[0] == 3 && d[1] < 4)) {
			LOG_ERR("which passes no data as a plain controller: it takes 3.4.0, "
				"see bnrg-update.conf");
		}
	}
	if (rsp != NULL) {
		net_buf_unref(rsp);
	}
}

static bool turn_on(void)
{
	static const uint8_t power[] = {1, 7}; /* +8 dBm, as the stock firmware */
	static bool registered;
	size_t count = 1;
	int ret;

	set_state(BLUETOOTH_STARTING);
	ret = set_address();
	if (ret == 0) {
		ret = bt_enable(NULL);
	}
	if (ret != 0) {
		LOG_ERR("the radio did not start: %d", ret);
		return false;
	}
	if (!registered) {
		bt_gatt_cb_register(&gatt_callbacks);
		bt_nus_cb_register(&nus_callbacks, NULL);
		registered = true;
	}
	bt_id_get(&own, &count);
	check_firmware();
	ret = vendor_cmd(HAL_SET_TX_POWER, power, sizeof(power), NULL);
	if (ret != 0) {
		LOG_WRN("transmit power not set: %d", ret);
	}
	return true;
}

static void closed(void)
{
	struct chunk chunk;

	while (k_msgq_get(&received, &chunk, K_NO_WAIT) == 0) {
	}
	phone_disconnected();
}

static void turn_off(void)
{
	struct bt_conn *was;
	int ret = bt_disable();

	if (ret != 0 && ret != -EALREADY) {
		LOG_ERR("the radio did not stop: %d", ret);
	}
	/* The stack drops a link without saying so when it is stopped. */
	K_SPINLOCK(&lock) {
		was = link;
		link = NULL;
		listening = false;
	}
	atomic_clear(todo);
	if (was != NULL) {
		bt_conn_unref(was);
		closed();
	}
}

/* The name in the advertisement, the service in what a scan is answered. */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};
static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SRV_VAL),
};

static void advertise(void)
{
	int ret = bt_le_adv_start(
		BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, ADVERTISE_MIN, ADVERTISE_MAX, NULL), ad,
		ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (ret == -EALREADY) {
		return;
	}
	if (ret != 0) {
		LOG_ERR("advertising did not start: %d", ret);
		set_state(BLUETOOTH_FAILED);
		return;
	}
	set_state(BLUETOOTH_ADVERTISING);
}

static void read_rssi(void)
{
	struct bt_conn *conn = link_ref();
	struct bt_hci_cp_read_rssi cp;
	struct net_buf *rsp = NULL;
	uint16_t handle;
	int8_t value = 0;
	int ret;

	rssi_at_ms = k_uptime_get();
	if (conn == NULL) {
		return;
	}
	ret = bt_hci_get_conn_handle(conn, &handle);
	if (ret == 0) {
		cp.handle = sys_cpu_to_le16(handle);
		ret = vendor_cmd(BT_HCI_OP_READ_RSSI, &cp, sizeof(cp), &rsp);
	}
	if (ret == 0 && rsp->len >= sizeof(struct bt_hci_rp_read_rssi)) {
		value = ((struct bt_hci_rp_read_rssi *)rsp->data)->rssi;
	}
	if (rsp != NULL) {
		net_buf_unref(rsp);
	}
	bt_conn_unref(conn);
	K_SPINLOCK(&lock) {
		rssi = value;
		rssi_status = ret;
	}
}

static void bluetooth_thread(void *p1, void *p2, void *p3)
{
	static struct chunk chunk;
	bool on = false;

	while (true) {
		k_timeout_t wait = K_FOREVER;

		if (atomic_test_and_clear_bit(todo, DO_RESTART) && on) {
			LOG_WRN("starting the radio over, as asked");
			turn_off();
			on = false;
			tried = false;
			K_SPINLOCK(&lock) {
				resets++;
			}
			set_state(BLUETOOTH_OFF);
		}
		if (set_on && !on && !tried) {
			tried = true;
			on = turn_on();
			if (on) {
				advertise();
			} else {
				set_state(BLUETOOTH_FAILED);
			}
		} else if (!set_on && (on || state != BLUETOOTH_OFF)) {
			if (on) {
				turn_off();
			}
			on = false;
			set_state(BLUETOOTH_OFF);
		}
		while (k_msgq_get(&received, &chunk, K_NO_WAIT) == 0) {
			phone_rx(chunk.data, chunk.len);
		}
		if (atomic_test_and_clear_bit(todo, DO_CLOSED)) {
			closed();
		}
		if (atomic_test_and_clear_bit(todo, DO_OPENED) && listening) {
			phone_connected();
		}
		if (atomic_test_and_clear_bit(todo, DO_ADVERTISE) && on && link == NULL) {
			advertise();
		}
		if (on && link != NULL) {
			int64_t left = rssi_at_ms + RSSI_EVERY_MS - k_uptime_get();

			if (left <= 0) {
				read_rssi();
				continue;
			}
			wait = K_MSEC(left);
		}
		k_sem_take(&wake, wait);
	}
}
K_THREAD_DEFINE(bluetooth_tid, 4096, bluetooth_thread, NULL, NULL, NULL, 7, 0, 3000);

/* radio [on|off|restart] */
static int cmd_radio(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const names[] = {"off", "starting", "advertising", "connected",
					    "failed"};
	struct bluetooth_status s;

	if (argc > 1 && strcmp(argv[1], "restart") == 0) {
		ask(DO_RESTART);
	} else if (argc > 1) {
		bluetooth_set(strcmp(argv[1], "on") == 0);
	}
	bluetooth_status(&s);
	shell_print(sh, "%s (set %s), own %s, peer %s%s", names[s.state],
		    bluetooth_is_set() ? "on" : "off", s.own[0] ? s.own : "-",
		    s.peer[0] ? s.peer : "none", s.listening ? ", serial line open" : "");
	shell_print(sh,
		    "interval %u ms, latency %u, mtu %u, connected %u s, %u connects, "
		    "%u started over, rx %u tx %u bytes, rssi %d dBm (status %d)",
		    s.interval_ms, s.latency, s.mtu, s.connected_s, s.connects, s.resets,
		    s.rx_bytes, s.tx_bytes, s.rssi, s.rssi_status);
	return 0;
}
SHELL_CMD_ARG_REGISTER(radio, NULL,
		       "Bluetooth state; turn it on or off, or start it over: radio [on|off|restart]",
		       cmd_radio, 1, 1);
