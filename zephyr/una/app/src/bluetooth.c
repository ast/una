/*
 * Bluetooth, with Zephyr's host stack. The watch is a peripheral with one
 * service of its own, the Nordic UART service, a serial line to the phone:
 * what the phone writes goes to phone_rx(), bluetooth_send() notifies it.
 *
 * The line is only for a phone that has paired: the watch shows six digits,
 * the phone's owner types them in (LE Secure Connections, passkey entry),
 * and the bond is kept in settings. A link that is not encrypted with such
 * a bond within a minute is dropped.
 *
 * The stack's callbacks run on its own threads, where nothing may wait. What
 * takes time is handed to a work queue of this file's, which is also the
 * only place the radio is turned on and off from.
 *
 * On the watch the controller is ST's BlueNRG-2N, told to be no more than a
 * controller (firmware 3.4.0 or later: see bnrg-update.conf). What is ST's
 * own is in chip_setup(); nothing else here knows the chip.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/bluetooth/services/nus/inst.h>
#include <zephyr/drivers/bluetooth.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/hash_function.h>
#include <zephyr/sys/util.h>

#include "bluetooth.h"
#include "pairing.h"
#include "phone.h"
#include "ui.h"

LOG_MODULE_REGISTER(bluetooth, LOG_LEVEL_INF);

/* A second between advertisements: found in a few seconds, at little cost. */
#define ADVERTISE_MIN 0x0640
#define ADVERTISE_MAX 0x0780

/* Encrypted, with a key the owner vouched for by typing the passkey. */
#define SECURE BT_SECURITY_L3
/* How long a phone has to get there: time enough to type six digits. */
#define SECURE_WITHIN K_SECONDS(60)

/*
 * A phone connects at a fast pace, which pairing and finding the services
 * are glad of. Once the line has been open for a while the watch asks for
 * the slow one it would like (CONFIG_BT_PERIPHERAL_PREF_*), which is what
 * the battery pays for all day.
 */
#define SLOW_AFTER K_SECONDS(15)

/*
 * How well the phone is heard, asked of the controller now and then. It is
 * in the log every minute: one that answers still has a link.
 */
#define RSSI_FIRST K_SECONDS(20)
#define RSSI_EVERY K_SECONDS(60)

/* After the rest of the watch has started. */
#define START_AFTER K_SECONDS(3)

/*
 * The serial service, as Zephyr's own instance of it but for the permissions:
 * to listen and to write takes an authenticated link.
 *
 * Services are laid out in the order of their names, and a phone that has
 * paired remembers where it found things. Nothing tells it of a change (see
 * CONFIG_BT_GATT_CACHING in boards/una_watch.conf), so this one has a name
 * that keeps it first after the stack's own two, wherever others are added.
 */
#define nus_service _3_serial_service
BT_GATT_SERVICE_DEFINE(nus_service,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_NUS_SERVICE),
	BT_GATT_CHARACTERISTIC(BT_UUID_NUS_TX_CHAR, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL,
			       NULL, NULL),
	BT_GATT_CCC(nus_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_AUTHEN),
	BT_GATT_CHARACTERISTIC(BT_UUID_NUS_RX_CHAR,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE_AUTHEN, NULL, nus_bt_chr_write, NULL),
);
static sys_slist_t nus_callback_list = SYS_SLIST_STATIC_INIT(&nus_callback_list);
STRUCT_SECTION_ITERABLE(bt_nus_inst, nus) = {
	.svc = &nus_service,
	.cbs = &nus_callback_list,
};
/* The characteristic the phone subscribes to. */
#define NUS_TX_ATTR (&nus_service.attrs[1])

/* The name in the advertisement, the service in what a scan is answered. */
static const struct bt_data advertisement[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};
static const struct bt_data scan_response[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SRV_VAL),
};

/* What the phone wrote, on its way from the stack's thread to the queue. */
struct chunk {
	uint16_t len;
	uint8_t data[CONFIG_BT_L2CAP_TX_MTU - 3];
};
K_MSGQ_DEFINE(received, sizeof(struct chunk), 8, 2);

static K_THREAD_STACK_DEFINE(queue_stack, 4096);
static struct k_work_q queue;

static void apply(struct k_work *work);
static void advertise(struct k_work *work);
static void receive(struct k_work *work);
static void tell_phone(struct k_work *work);
static void forget(struct k_work *work);
static void read_rssi(struct k_work *work);
static void check_secure(struct k_work *work);
static void slow_down(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(apply_work, apply);
static K_WORK_DEFINE(advertise_work, advertise);
static K_WORK_DEFINE(receive_work, receive);
static K_WORK_DEFINE(tell_phone_work, tell_phone);
static K_WORK_DEFINE(forget_work, forget);
static K_WORK_DELAYABLE_DEFINE(rssi_work, read_rssi);
static K_WORK_DELAYABLE_DEFINE(secure_work, check_secure);
static K_WORK_DELAYABLE_DEFINE(slow_work, slow_down);

static uint8_t set_on = 1;
static atomic_t restart_wanted;
static atomic_t link_closed;
/* Of the queue alone: whether the stack runs. */
static bool on;

static struct k_spinlock lock;
static enum bluetooth_state state = BLUETOOTH_OFF;
static struct bt_conn *link;
static bool listening;
static uint8_t security;
static uint16_t interval_ms, latency, mtu;
static bt_addr_le_t peer, own;
static int64_t connected_at_ms;
static uint32_t connects, timeouts, resets, rx_bytes, tx_bytes;
static int rssi_status;
static int8_t rssi;

static void set_state(enum bluetooth_state to)
{
	K_SPINLOCK(&lock) {
		state = to;
	}
	ui_refresh();
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

/* The public face */

static void count_bond(const struct bt_bond_info *info, void *count)
{
	(*(uint8_t *)count)++;
}

void bluetooth_status(struct bluetooth_status *status)
{
	memset(status, 0, sizeof(*status));
	K_SPINLOCK(&lock) {
		status->state = state;
		status->listening = listening;
		status->connects = connects;
		status->timeouts = timeouts;
		status->resets = resets;
		status->rx_bytes = rx_bytes;
		status->tx_bytes = tx_bytes;
		if (state == BLUETOOTH_ADVERTISING || state == BLUETOOTH_CONNECTED) {
			bt_addr_to_str(&own.a, status->own, sizeof(status->own));
		}
		if (state == BLUETOOTH_CONNECTED) {
			bt_addr_to_str(&peer.a, status->peer, sizeof(status->peer));
			status->secure = security >= SECURE;
			status->interval_ms = interval_ms;
			status->latency = latency;
			status->mtu = mtu;
			status->connected_s = (k_uptime_get() - connected_at_ms) / 1000;
			status->rssi = rssi;
			status->rssi_status = rssi_status;
		}
	}
	if (status->state != BLUETOOTH_OFF && status->state != BLUETOOTH_FAILED) {
		bt_foreach_bond(BT_ID_DEFAULT, count_bond, &status->bonds);
	}
}

bool bluetooth_is_set(void)
{
	return set_on;
}

void bluetooth_set(bool to)
{
	set_on = to;
	settings_save_one("radio/on", &set_on, sizeof(set_on));
	k_work_reschedule_for_queue(&queue, &apply_work, K_NO_WAIT);
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

void bluetooth_forget(void)
{
	k_work_submit_to_queue(&queue, &forget_work);
}

bool bluetooth_send(const void *data, size_t len)
{
	struct bt_conn *conn = link_ref();
	const uint8_t *at = data;
	bool sent = conn != NULL && listening;

	while (sent && len > 0) {
		size_t n = MIN(len, bt_gatt_get_mtu(conn) - 3);
		int ret = bt_nus_inst_send(conn, &nus, at, n);

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

/* The stack's callbacks: note what happened, leave the rest to the queue. */

/* The phone listens when it has subscribed over a link that is secure. */
static void update_listening(struct bt_conn *conn)
{
	bool now = bt_conn_get_security(conn) >= SECURE &&
		   bt_gatt_is_subscribed(conn, NUS_TX_ATTR, BT_GATT_CCC_NOTIFY);

	K_SPINLOCK(&lock) {
		listening = now && conn == link;
	}
	k_work_submit_to_queue(&queue, &tell_phone_work);
	ui_refresh();
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;
	char str[BT_ADDR_LE_STR_LEN];

	if (err != 0) {
		LOG_WRN("a connection failed: 0x%02x %s", err, bt_hci_err_to_str(err));
		return;
	}
	if (bt_conn_get_info(conn, &info) != 0) {
		memset(&info, 0, sizeof(info));
	}
	K_SPINLOCK(&lock) {
		link = bt_conn_ref(conn);
		listening = false;
		security = info.security.level;
		peer = *bt_conn_get_dst(conn);
		mtu = 23; /* until the phone asks for more */
		interval_ms = info.le.interval_us / 1000;
		latency = info.le.latency;
		connected_at_ms = k_uptime_get();
		rssi = 0;
		rssi_status = 0;
		connects++;
		state = BLUETOOTH_CONNECTED;
	}
	bt_addr_le_to_str(&peer, str, sizeof(str));
	LOG_INF("connected to %s, interval %u ms", str, interval_ms);
	/*
	 * Security is the phone's to start: one with a bond encrypts by itself,
	 * one without is refused the serial line and pairs to get it. Asking
	 * for it from here as well collides with a phone that has begun, and
	 * Android then never tells its apps of the link.
	 */
	k_work_reschedule_for_queue(&queue, &secure_work, SECURE_WITHIN);
	k_work_reschedule_for_queue(&queue, &rssi_work, RSSI_FIRST);
	ui_refresh();
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct bt_conn *was = NULL;

	K_SPINLOCK(&lock) {
		if (conn == link) {
			was = link;
			link = NULL;
			listening = false;
			security = 0;
			state = BLUETOOTH_STARTING;
			if (reason == BT_HCI_ERR_CONN_TIMEOUT) {
				timeouts++;
			}
		}
	}
	if (was == NULL) {
		return;
	}
	bt_conn_unref(was);
	LOG_INF("disconnected: 0x%02x %s", reason, bt_hci_err_to_str(reason));
	pairing_hide();
	atomic_set(&link_closed, 1);
	k_work_submit_to_queue(&queue, &tell_phone_work);
	ui_refresh();
}

/* The connection is free to be had again: only now can advertising start. */
static void on_recycled(void)
{
	k_work_submit_to_queue(&queue, &advertise_work);
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

static void on_security_changed(struct bt_conn *conn, bt_security_t level,
				enum bt_security_err err)
{
	if (err != BT_SECURITY_ERR_SUCCESS) {
		/* Most often a bond that one side has and the other has forgotten. */
		LOG_WRN("not secured: %d %s", err, bt_security_err_to_str(err));
		bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
		return;
	}
	K_SPINLOCK(&lock) {
		if (conn == link) {
			security = level;
			/* Known by its own address now, if it came under another. */
			peer = *bt_conn_get_dst(conn);
		}
	}
	LOG_INF("security level %d", level);
	update_listening(conn);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.recycled = on_recycled,
	.le_param_updated = on_param_updated,
	.security_changed = on_security_changed,
};

static void on_mtu(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	uint16_t now = bt_gatt_get_mtu(conn);

	K_SPINLOCK(&lock) {
		mtu = now;
	}
}

static struct bt_gatt_cb gatt_callbacks = {.att_mtu_updated = on_mtu};

static void on_subscribed(bool enabled, void *ctx)
{
	struct bt_conn *conn = link_ref();

	if (conn != NULL) {
		update_listening(conn);
		bt_conn_unref(conn);
	}
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
	k_work_submit_to_queue(&queue, &receive_work);
}

static struct bt_nus_cb nus_callbacks = {
	.notif_enabled = on_subscribed,
	.received = on_received,
};

/* Pairing: the watch can show a passkey and nothing else. */

static void cancel_pairing(void)
{
	struct bt_conn *conn = link_ref();

	if (conn != NULL) {
		bt_conn_auth_cancel(conn);
		bt_conn_unref(conn);
	}
}

static void on_passkey(struct bt_conn *conn, unsigned int passkey)
{
	/* Not logged: it is for the eyes of whoever holds the watch. */
	LOG_INF("pairing: the passkey is on the screen");
	pairing_show(passkey, cancel_pairing);
}

static void on_pairing_cancelled(struct bt_conn *conn)
{
	LOG_INF("pairing given up");
	pairing_hide();
}

static struct bt_conn_auth_cb auth_callbacks = {
	.passkey_display = on_passkey,
	.cancel = on_pairing_cancelled,
};

static void on_paired(struct bt_conn *conn, bool bonded)
{
	LOG_INF("paired%s", bonded ? ", and the bond kept" : "");
	pairing_hide();
}

static void on_pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	LOG_WRN("pairing failed: %d %s", reason, bt_security_err_to_str(reason));
	pairing_hide();
}

static struct bt_conn_auth_info_cb auth_info_callbacks = {
	.pairing_complete = on_paired,
	.pairing_failed = on_pairing_failed,
};

/* The queue's work */

/*
 * The address to go by: a static random one made of the SoC's serial number,
 * the same at every start, so that a phone's bond holds. Set before the
 * stack starts, which then takes no other.
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

static int hci_cmd(uint16_t opcode, const void *params, size_t len, struct net_buf **rsp)
{
	struct net_buf *buf = bt_hci_cmd_alloc(K_SECONDS(1));

	if (buf == NULL) {
		return -ENOBUFS;
	}
	net_buf_add_mem(buf, params, len);
	return bt_hci_cmd_send_sync(opcode, buf, rsp);
}

#if defined(CONFIG_BT_BLUENRG_ACI)
/* ST's own commands, which the chip takes as a plain controller too. */
#define HAL_FIRMWARE_DETAILS BT_OP(BT_OGF_VS, 0x001)
#define HAL_SET_TX_POWER     BT_OP(BT_OGF_VS, 0x00f)

/*
 * When a phone connects under an address the chip cannot resolve, which is
 * every phone until it has paired, the chip's "enhanced connection complete"
 * has that address in the field for a resolved peer's and all ones where it
 * belongs. Pairing computes with the address and fails on such a one. So the
 * events pass through here on their way from the driver to the host, and
 * this one is put right.
 */
static bt_hci_recv_t host_recv;

static int chip_recv(const struct device *dev, struct net_buf *buf)
{
	struct {
		uint8_t type;
		struct bt_hci_evt_hdr hdr;
		struct bt_hci_evt_le_meta_event meta;
		struct bt_hci_evt_le_enh_conn_complete evt;
	} __packed *p = (void *)buf->data;

	if (buf->len >= sizeof(*p) && p->type == BT_HCI_H4_EVT &&
	    p->hdr.evt == BT_HCI_EVT_LE_META_EVENT &&
	    p->meta.subevent == BT_HCI_EVT_LE_ENH_CONN_COMPLETE &&
	    bt_addr_eq(&p->evt.peer_addr.a, BT_ADDR_NONE) &&
	    !bt_addr_eq(&p->evt.peer_rpa, BT_ADDR_ANY)) {
		bt_addr_copy(&p->evt.peer_addr.a, &p->evt.peer_rpa);
		bt_addr_copy(&p->evt.peer_rpa, BT_ADDR_ANY);
	}
	return host_recv(dev, buf);
}

/* The BlueNRG-2N: which firmware it has, for the log, and its power. */
static void chip_setup(void)
{
	static const uint8_t power[] = {1, 7}; /* +8 dBm, as the stock firmware */
	struct bt_hci_driver_data *driver = DEVICE_DT_GET(DT_CHOSEN(zephyr_bt_hci))->data;
	struct net_buf *rsp = NULL;
	const uint8_t *d;
	int ret;

	/* The driver is given the host's function anew at every start. */
	if (driver->recv != chip_recv) {
		host_recv = driver->recv;
		driver->recv = chip_recv;
	}

	if (hci_cmd(HAL_FIRMWARE_DETAILS, NULL, 0, &rsp) != 0 || rsp->len < 10) {
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
	ret = hci_cmd(HAL_SET_TX_POWER, power, sizeof(power), NULL);
	if (ret != 0) {
		LOG_WRN("transmit power not set: %d", ret);
	}
}
#else
static void chip_setup(void)
{
}
#endif

static bool turn_on(void)
{
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
		bt_nus_inst_cb_register(&nus, &nus_callbacks, NULL);
		bt_conn_auth_cb_register(&auth_callbacks);
		bt_conn_auth_info_cb_register(&auth_info_callbacks);
		registered = true;
	}
	/* The bonds, and what their phones had subscribed to. */
	ret = settings_load_subtree("bt");
	if (ret != 0) {
		LOG_WRN("bonds not loaded: %d", ret);
	}
	bt_id_get(&own, &count);
	chip_setup();
	return true;
}

static void turn_off(void)
{
	struct k_work_sync sync;
	struct bt_conn *was;
	int ret = bt_disable();

	if (ret != 0 && ret != -EALREADY) {
		LOG_ERR("the radio did not stop: %d", ret);
	}
	k_work_cancel_delayable_sync(&rssi_work, &sync);
	k_work_cancel_delayable_sync(&secure_work, &sync);
	k_work_cancel_delayable_sync(&slow_work, &sync);
	/* The stack drops a link without saying so when it is stopped. */
	K_SPINLOCK(&lock) {
		was = link;
		link = NULL;
		listening = false;
		security = 0;
	}
	if (was != NULL) {
		bt_conn_unref(was);
		atomic_set(&link_closed, 1);
	}
	pairing_hide();
	tell_phone(NULL);
}

/* The radio as it is set to be, and started over when that is asked for. */
static void apply(struct k_work *work)
{
	if (atomic_clear(&restart_wanted) && on) {
		LOG_WRN("starting the radio over, as asked");
		turn_off();
		on = false;
		K_SPINLOCK(&lock) {
			resets++;
		}
	}
	if (set_on && !on) {
		on = turn_on();
		if (on) {
			advertise(NULL);
		} else {
			set_state(BLUETOOTH_FAILED);
		}
	} else if (!set_on) {
		if (on) {
			turn_off();
			on = false;
		}
		set_state(BLUETOOTH_OFF);
	}
}

static void advertise(struct k_work *work)
{
	int ret;

	if (!on || link != NULL) {
		return;
	}
	ret = bt_le_adv_start(
		BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, ADVERTISE_MIN, ADVERTISE_MAX, NULL),
		advertisement, ARRAY_SIZE(advertisement), scan_response,
		ARRAY_SIZE(scan_response));
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

static void receive(struct k_work *work)
{
	static struct chunk chunk;

	while (k_msgq_get(&received, &chunk, K_NO_WAIT) == 0) {
		phone_rx(chunk.data, chunk.len);
	}
}

/* The line opening and closing, each told to the phone's side once. */
static void tell_phone(struct k_work *work)
{
	static bool open;

	if (atomic_clear(&link_closed) && open) {
		open = false;
		k_work_cancel_delayable(&slow_work);
		k_msgq_purge(&received);
		phone_disconnected();
	}
	if (listening && !open) {
		open = true;
		k_work_reschedule_for_queue(&queue, &slow_work, SLOW_AFTER);
		phone_connected();
	}
}

static void slow_down(struct k_work *work)
{
	struct bt_conn *conn = link_ref();
	int ret;

	if (conn == NULL) {
		return;
	}
	/* A phone that says no keeps its own pace. */
	ret = bt_conn_le_param_update(
		conn, BT_LE_CONN_PARAM(CONFIG_BT_PERIPHERAL_PREF_MIN_INT,
				       CONFIG_BT_PERIPHERAL_PREF_MAX_INT,
				       CONFIG_BT_PERIPHERAL_PREF_LATENCY,
				       CONFIG_BT_PERIPHERAL_PREF_TIMEOUT));
	if (ret != 0 && ret != -EALREADY) {
		LOG_WRN("slow pace not asked for: %d", ret);
	}
	bt_conn_unref(conn);
}

static void forget(struct k_work *work)
{
	/* Their links go with them. */
	int ret = on ? bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY) : -ENETDOWN;

	if (ret != 0) {
		LOG_WRN("bonds not forgotten: %d", ret);
	} else {
		LOG_INF("all bonds forgotten");
	}
	ui_refresh();
}

static void read_rssi(struct k_work *work)
{
	struct bt_conn *conn = link_ref();
	struct bt_hci_cp_read_rssi cp;
	struct net_buf *rsp = NULL;
	uint16_t handle;
	int8_t value = 0;
	int ret;

	if (conn == NULL) {
		return;
	}
	ret = bt_hci_get_conn_handle(conn, &handle);
	if (ret == 0) {
		cp.handle = sys_cpu_to_le16(handle);
		ret = hci_cmd(BT_HCI_OP_READ_RSSI, &cp, sizeof(cp), &rsp);
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
	k_work_reschedule_for_queue(&queue, &rssi_work, RSSI_EVERY);
}

/* A link that has not been secured by now is not the owner's phone. */
static void check_secure(struct k_work *work)
{
	struct bt_conn *conn = link_ref();

	if (conn == NULL) {
		return;
	}
	if (bt_conn_get_security(conn) < SECURE) {
		LOG_WRN("not secured in time: dropping the link");
		bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
	}
	bt_conn_unref(conn);
}

static int bluetooth_init(void)
{
	static const struct k_work_queue_config config = {.name = "bluetooth"};

	k_work_queue_start(&queue, queue_stack, K_THREAD_STACK_SIZEOF(queue_stack), 7, &config);
	k_work_schedule_for_queue(&queue, &apply_work, START_AFTER);
	return 0;
}
SYS_INIT(bluetooth_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* radio [on|off|restart|forget|pace MIN_MS MAX_MS LATENCY TIMEOUT_MS] */
static int cmd_radio(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const names[] = {"off", "starting", "advertising", "connected",
					    "failed"};
	struct bluetooth_status s;

	if (argc > 1 && strcmp(argv[1], "restart") == 0) {
		atomic_set(&restart_wanted, 1);
		k_work_reschedule_for_queue(&queue, &apply_work, K_NO_WAIT);
	} else if (argc > 1 && strcmp(argv[1], "forget") == 0) {
		bluetooth_forget();
	} else if (argc == 6 && strcmp(argv[1], "pace") == 0) {
		/* For trying things: milliseconds, but for the latency. */
		struct bt_conn *conn = link_ref();
		int ret = -ENOTCONN;

		if (conn != NULL) {
			ret = bt_conn_le_param_update(
				conn, BT_LE_CONN_PARAM(atoi(argv[2]) * 4 / 5, atoi(argv[3]) * 4 / 5,
						       atoi(argv[4]), atoi(argv[5]) / 10));
			bt_conn_unref(conn);
		}
		shell_print(sh, "asked: %d", ret);
	} else if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
		bluetooth_set(strcmp(argv[1], "on") == 0);
	} else if (argc > 1) {
		shell_error(sh, "radio [on|off|restart|forget|pace MIN MAX LATENCY TIMEOUT]");
		return -EINVAL;
	}
	bluetooth_status(&s);
	shell_print(sh, "%s (set %s), own %s, peer %s%s%s, %u paired", names[s.state],
		    bluetooth_is_set() ? "on" : "off", s.own[0] ? s.own : "-",
		    s.peer[0] ? s.peer : "none",
		    s.state != BLUETOOTH_CONNECTED ? "" : s.secure ? ", secure" : ", not secure",
		    s.listening ? ", serial line open" : "", s.bonds);
	shell_print(sh,
		    "interval %u ms, latency %u, mtu %u, connected %u s, %u connects, "
		    "%u lost, %u started over, rx %u tx %u bytes, rssi %d dBm (status %d)",
		    s.interval_ms, s.latency, s.mtu, s.connected_s, s.connects, s.timeouts, s.resets,
		    s.rx_bytes, s.tx_bytes, s.rssi, s.rssi_status);
	return 0;
}
SHELL_CMD_ARG_REGISTER(radio, NULL,
		       "Bluetooth state; turn it on or off, start it over, or forget the "
		       "phones paired: radio [on|off|restart|forget|pace MIN MAX LATENCY "
		       "TIMEOUT]",
		       cmd_radio, 1, 5);
