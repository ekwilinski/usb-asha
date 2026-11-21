#include "asha.h"
#include "indicator.h"
#ifdef CONFIG_USB_ASHA_POWER_BUTTON
#include "dongle_controls.h"
#include "radio_power.h"
BUILD_ASSERT(CONFIG_BT_CTLR_TX_PWR_DBM == 4 && IS_ENABLED(CONFIG_BT_CTLR_TX_PWR_DYNAMIC_CONTROL));
static atomic_t selected_dbm = CONFIG_BT_CTLR_TX_PWR_DBM;
#endif
#ifdef CONFIG_USB_ASHA_SONNET3
#include "sonnet_mix.h"
#endif
#include "packet_timing.h"
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

enum characteristic { PROPERTIES, CONTROL, STATUS, VOLUME, PSM, CHAR_COUNT };
#define UUID(a,b,c,d,e) BT_UUID_INIT_128(BT_UUID_128_ENCODE(a,b,c,d,e))
static const struct bt_uuid_128 uuid[CHAR_COUNT] = {
	[PROPERTIES] = UUID(0x6333651e,0xc481,0x4a3e,0x9169,0x7c902aad37bb),
	[CONTROL] = UUID(0xf0d4de7e,0x4a88,0x476c,0x9d9f,0x1937b0996cc0),
	[STATUS] = UUID(0x38663f1a,0xe711,0x4cac,0xb641,0x326b56404837),
	[VOLUME] = UUID(0x00e4ca9e,0xab14,0x41e4,0x8823,0xf9e70c7e91df),
	[PSM] = UUID(0x2d410339,0x82b6,0x42aa,0xb34e,0xe2e01df8cc1a),
};
static const struct bt_uuid_128 sonnet_service = UUID(0x5f3b2e9a,0x961f,0x1883,0xd24e,0x59d8b0f44f78);
static const struct bt_uuid_16 service = BT_UUID_INIT_16(0xfdf0);
enum event { CONNECTED = BIT(0), ENCRYPTED = BIT(1), DONE = BIT(2), ACK = BIT(3),
	     OPEN = BIT(4), FAILED = BIT(5), DOWN = BIT(6), TIMING = BIT(7) };
enum stage { IDLE, CONNECT, SECURE, DISCOVER, READ_PROPERTIES, READ_PSM, SUBSCRIBE,
	     OPEN_CHANNEL, READY, START, STREAMING, STOP };

struct peer {
	struct k_event events;
	struct bt_conn *conn;
	struct bt_l2cap_le_chan channel;
	struct bt_gatt_discover_params discovery, ccc_discovery;
	struct bt_gatt_read_params read;
	struct bt_gatt_write_params write;
	struct bt_gatt_subscribe_params subscription;
	uint16_t first, last, handle[CHAR_COUNT], status_ccc, psm;
	uint8_t props[17], read_length, command[5];
	bool ready, started, temporary_bond;
	int volume;
};
static struct peer peers[SIDES];
BUILD_ASSERT(ARRAY_SIZE(peers) == CONFIG_BT_MAX_CONN);
NET_BUF_POOL_FIXED_DEFINE(audio_buffers, SIDES * 8, BT_L2CAP_SDU_BUF_SIZE(1 + CODE_BYTES),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);
K_MSGQ_DEFINE(candidates, sizeof(bt_addr_le_t), 1, 1);
static bool scanning, playing;
static atomic_t pairing;
static int64_t pair_until, scan_after;
static uint8_t sequence;
static uint16_t stream;
static bool sent_first_frame;

static struct peer *link(struct bt_conn *conn) { return &peers[bt_conn_index(conn)]; }

static void fail(struct peer *p, int reason)
{
	atomic_set(&diag_error, reason);
	k_event_post(&p->events, FAILED);
}

static int wait_for(struct peer *p, uint32_t needed, int timeout_ms)
{
	int64_t until = k_uptime_get() + timeout_ms;
	while (needed) {
		int64_t remaining = until - k_uptime_get();
		if (remaining <= 0) {
			return -ETIMEDOUT;
		}
		uint32_t got = k_event_wait(&p->events, needed | FAILED | DOWN, false, K_MSEC(remaining));
		if (!got) { return -ETIMEDOUT; }
		if (got & DOWN) { return -ENOTCONN; }
		if (got & FAILED) { return -EIO; }
		needed &= ~got;
	}
	return 0;
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	k_event_post(&link(conn)->events, err ? DOWN : CONNECTED);
}
static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	atomic_inc(&diag_disconnects);
	if (reason != BT_HCI_ERR_LOCALHOST_TERM_CONN) { atomic_set(&diag_error, reason); }
	k_event_post(&link(conn)->events, DOWN);
}
static void secured(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	struct peer *p = link(conn);
	if (err || level < BT_SECURITY_L2) { fail(p, err ? err : -EACCES); }
	else { k_event_post(&p->events, ENCRYPTED); }
}
static bool params_requested(struct bt_conn *conn, struct bt_le_conn_param *params)
{
	if (params->interval_min > 16 || params->interval_max < 16) { return false; }
	params->interval_min = params->interval_max = 16;
	params->latency = 0;
	return true;
}
static void params_changed(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout)
{
	struct peer *p = link(conn);
	if (interval != 16 || latency) { fail(p, -ERANGE); }
	else { k_event_post(&p->events, TIMING); }
}
BT_CONN_CB_DEFINE(connection_events) = {
	.connected = connected, .disconnected = disconnected, .security_changed = secured,
	.le_param_req = params_requested, .le_param_updated = params_changed,
};

static void channel_open(struct bt_l2cap_chan *chan)
{
	struct peer *p = CONTAINER_OF(chan, struct peer, channel.chan);
	timing_channel_open(p - peers);
	if (p->channel.tx.mtu < CODE_BYTES + 1) { fail(p, -EMSGSIZE); }
	else { k_event_post(&p->events, OPEN); }
}
static void channel_closed(struct bt_l2cap_chan *chan)
{
	fail(CONTAINER_OF(chan, struct peer, channel.chan), -ENOTCONN);
}
static int channel_receive(struct bt_l2cap_chan *chan, struct net_buf *buf) { return 0; }
static void channel_sent(struct bt_l2cap_chan *chan)
{
	struct peer *p = CONTAINER_OF(chan, struct peer, channel.chan);
	timing_complete(p - peers);
}
static const struct bt_l2cap_chan_ops channel_ops = {
	.sent = channel_sent,
	.connected = channel_open, .disconnected = channel_closed, .recv = channel_receive,
};

static uint8_t discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   struct bt_gatt_discover_params *params)
{
	struct peer *p = link(conn);
	if (!attr) {
		k_event_post(&p->events, DONE);
		return BT_GATT_ITER_STOP;
	}
	if (params->type == BT_GATT_DISCOVER_PRIMARY) {
		const struct bt_gatt_service_val *s = attr->user_data;
		p->first = attr->handle + 1;
		p->last = s->end_handle;
		k_event_post(&p->events, DONE);
		return BT_GATT_ITER_STOP;
	}
	const struct bt_gatt_chrc *ch = attr->user_data;
	for (int i = 0; i < CHAR_COUNT; ++i) {
		if (!bt_uuid_cmp(ch->uuid, &uuid[i].uuid)) {
			p->handle[i] = ch->value_handle;
			if (i == STATUS) {
				p->status_ccc = ch->properties & BT_GATT_CHRC_NOTIFY ? BT_GATT_CCC_NOTIFY :
					ch->properties & BT_GATT_CHRC_INDICATE ? BT_GATT_CCC_INDICATE : 0;
			}
		}
	}
	return BT_GATT_ITER_CONTINUE;
}

static int discover(struct peer *p, bool primary)
{
	p->discovery = (struct bt_gatt_discover_params){
		.uuid = primary ? &service.uuid : NULL, .func = discover_cb,
		.start_handle = primary ? BT_ATT_FIRST_ATTRIBUTE_HANDLE : p->first,
		.end_handle = primary ? BT_ATT_LAST_ATTRIBUTE_HANDLE : p->last,
		.type = primary ? BT_GATT_DISCOVER_PRIMARY : BT_GATT_DISCOVER_CHARACTERISTIC,
	};
	k_event_clear(&p->events, DONE);
	int err = bt_gatt_discover(p->conn, &p->discovery);
	return err ? err : wait_for(p, DONE, 6000);
}

static uint8_t read_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_read_params *params,
		       const void *data, uint16_t len)
{
	struct peer *p = link(conn);
	if (err || !data || len != p->read_length) {
		fail(p, err ? err : -EBADMSG);
	} else {
		if (len == sizeof(p->props)) { memcpy(p->props, data, len); }
		else { p->psm = sys_get_le16(data); }
		k_event_post(&p->events, DONE);
	}
	return BT_GATT_ITER_STOP;
}
static int read_value(struct peer *p, enum characteristic key, uint8_t length)
{
	p->read_length = length;
	p->read = (struct bt_gatt_read_params){.func = read_cb, .handle_count = 1,
					      .single.handle = p->handle[key]};
	k_event_clear(&p->events, DONE);
	int err = bt_gatt_read(p->conn, &p->read);
	return err ? err : wait_for(p, DONE, 5000);
}

static void subscribed(struct bt_conn *conn, uint8_t err, struct bt_gatt_subscribe_params *params)
{
	if (err) { fail(link(conn), err); }
	else { k_event_post(&link(conn)->events, DONE); }
}
static uint8_t notified(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
			const void *data, uint16_t len)
{
	if (!data) { return BT_GATT_ITER_STOP; }
	struct peer *p = link(conn);
	if (len != 1 || *(const uint8_t *)data != 0) {
		fail(p, len == 1 ? (int8_t)*(const uint8_t *)data : -EBADMSG);
	} else {
		k_event_post(&p->events, ACK);
	}
	return BT_GATT_ITER_CONTINUE;
}
static void wrote(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *params)
{
	if (err) { fail(link(conn), err); }
	else { k_event_post(&link(conn)->events, DONE); }
}

static int control(struct peer *p, bool start, bool paired)
{
	atomic_set(&diag_stage, start ? START : STOP);
	p->volume = audio_volume();
	p->command[0] = start ? 1 : 2;
	p->command[1] = 1;
	p->command[2] = 3;
	p->command[3] = (uint8_t)p->volume;
	p->command[4] = paired;
	p->write = (struct bt_gatt_write_params){.func = wrote, .handle = p->handle[CONTROL],
		.data = p->command, .length = start ? 5 : 1};
	k_event_clear(&p->events, DONE | ACK);
	int err = bt_gatt_write(p->conn, &p->write);
	return err ? err : wait_for(p, DONE | ACK, 3000);
}

static int setup(struct peer *p)
{
	atomic_set(&diag_stage, SECURE);
	int err = bt_conn_set_security(p->conn, BT_SECURITY_L2);
	if (err) { return err; }
	if (bt_conn_get_security(p->conn) < BT_SECURITY_L2 && (err = wait_for(p, ENCRYPTED, 20000))) {
		return err;
	}
	atomic_set(&diag_stage, DISCOVER);
	if ((err = discover(p, true)) || !p->first) { return err ? err : -ENOENT; }
	if ((err = discover(p, false))) { return err; }
	for (unsigned i = 0; i < CHAR_COUNT; i++) {
		if (!p->handle[i]) { return -ENOENT; }
	}
	if (!p->status_ccc) { return -ENOTSUP; }
	atomic_set(&diag_stage, READ_PROPERTIES);
	if ((err = read_value(p, PROPERTIES, sizeof(p->props)))) { return err; }
	if (p->props[0] != 1 || !(p->props[10] & 1) || !(sys_get_le16(p->props + 15) & 2)) {
		return -ENOTSUP;
	}
	for (unsigned i = 0; i < SIDES; i++) {
		struct peer *other = &peers[i];
		if (other->ready && (!(other->props[1] & p->props[1] & 2) ||
		    !((other->props[1] ^ p->props[1]) & 1) ||
		    memcmp(other->props + 2, p->props + 2, 8))) {
			return -EXDEV;
		}
	}
	atomic_set(&diag_stage, READ_PSM);
	if ((err = read_value(p, PSM, 2))) { return err; }
	if (p->psm < 0x80 || p->psm > 0xff) { return -EBADMSG; }
	atomic_set(&diag_stage, SUBSCRIBE);
	p->subscription = (struct bt_gatt_subscribe_params){.notify = notified, .subscribe = subscribed,
		.value_handle = p->handle[STATUS], .end_handle = p->last, .value = p->status_ccc,
		.disc_params = &p->ccc_discovery, .min_security = BT_SECURITY_L2};
	atomic_set_bit(p->subscription.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
	k_event_clear(&p->events, DONE);
	err = bt_gatt_subscribe(p->conn, &p->subscription);
	if (err || (err = wait_for(p, DONE, 5000))) { return err; }
	atomic_set(&diag_stage, OPEN_CHANNEL);
	p->channel.chan.ops = &channel_ops;
	p->channel.required_sec_level = BT_SECURITY_L2;
	err = bt_l2cap_chan_connect(p->conn, &p->channel.chan, p->psm);
	if (err || (err = wait_for(p, OPEN, 5000))) { return err; }
	struct bt_conn_info info;
	if ((err = bt_conn_get_info(p->conn, &info))) { return err; }
	if (info.le.interval != 16 || info.le.latency != 0) { return -ERANGE; }
	p->temporary_bond = false;
	p->ready = true;
	atomic_set(&diag_stage, READY);
	return 0;
}

static bool advertised(struct bt_data *ad, void *user)
{
	bool *found = user;
	switch (ad->type) {
	case BT_DATA_SVC_DATA16:
		*found = ad->data_len >= 2 && sys_get_le16(ad->data) == service.val;
		break;
	case BT_DATA_UUID16_ALL:
	case BT_DATA_UUID16_SOME:
		for (unsigned i = 0; i + 2 <= ad->data_len; i += 2) {
			if (sys_get_le16(ad->data + i) == service.val) { *found = true; break; }
		}
		break;
	case BT_DATA_UUID128_ALL:
	case BT_DATA_UUID128_SOME:
	case BT_DATA_SVC_DATA128:
		for (unsigned i = 0; i + 16 <= ad->data_len; i += 16) {
			if (!memcmp(ad->data + i, sonnet_service.val, 16)) { *found = true; break; }
			if (ad->type == BT_DATA_SVC_DATA128) { break; }
		}
		break;
	case BT_DATA_NAME_COMPLETE:
	case BT_DATA_NAME_SHORTENED:
		/* Some SONNET advertising modes announce the product name first. */
		*found = ad->data_len >= 8 && !memcmp(ad->data, "SONNET 3", 8);
		break;
	}
	return !*found;
}

static void found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *ad)
{
	if (type != BT_GAP_ADV_TYPE_ADV_IND && type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND &&
	    type != BT_GAP_ADV_TYPE_SCAN_RSP) { return; }
	atomic_inc(&diag_advertisements);
	bool eligible = bt_le_bond_exists(BT_ID_DEFAULT, addr);
	if (!eligible && atomic_get(&pairing)) { bt_data_parse(ad, advertised, &eligible); }
	if (eligible) {
		atomic_inc(&diag_matches);
		k_msgq_put(&candidates, addr, K_NO_WAIT);
	}
}

static void stop_scan(void)
{
	if (scanning) {
		bt_le_scan_stop();
		scanning = false;
	}
	k_msgq_purge(&candidates);
}
static void drop(struct peer *p, int err)
{
	if (!k_event_test(&p->events, FAILED)) { atomic_set(&diag_error, err); }
	k_event_post(&p->events, FAILED);
	p->ready = false;
	p->started = false;
	bt_conn_disconnect(p->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	scan_after = k_uptime_get() + 2000;
}

unsigned asha_ready(void)
{
	unsigned count = 0;
	for (unsigned i = 0; i < SIDES; ++i) { count += peers[i].ready; }
	return count;
}
bool asha_pairing(void) { return atomic_get(&pairing); }
bool asha_playing(void) { return playing; }

void asha_stop(void)
{
	playing = false;
	for (unsigned i = 0; i < SIDES; ++i) {
		struct peer *p = &peers[i];
		if (p->started) {
			p->started = false;
			int err = control(p, false, false);
			if (err) { drop(p, err); }
		}
	}
	audio_flush();
}

static void tell_other(struct peer *changed, uint8_t state)
{
	const uint8_t status[] = {3, state};
	for (unsigned i = 0; i < SIDES; ++i) {
		struct peer *p = &peers[i];
		if (p == changed || !p->ready) { continue; }
		int err = bt_gatt_write_without_response(p->conn, p->handle[CONTROL], status, sizeof(status), false);
		if (err) { drop(p, err); }
	}
}

static void attach(const bt_addr_le_t *addr)
{
	bool known = bt_le_bond_exists(BT_ID_DEFAULT, addr);
	if (!known && !asha_pairing()) { return; }
	struct bt_conn *existing = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr);
	if (existing) { bt_conn_unref(existing); return; }
	stop_scan();
	asha_stop();
	indicator_set(LED_CONNECT);
	atomic_set(&diag_stage, CONNECT);
	atomic_set(&diag_error, 0);
	struct bt_conn_le_create_param create =
		BT_CONN_LE_CREATE_PARAM_INIT(BT_CONN_LE_OPT_NONE, 96, 48);
	create.timeout = 500; /* 5 s in 10 ms units */
	struct bt_conn *conn = NULL;
	int err = bt_conn_le_create(addr, &create, BT_LE_CONN_PARAM(16, 16, 0, 400), &conn);
	if (err) {
		atomic_set(&diag_error, err);
		scan_after = k_uptime_get() + 500;
		return;
	}
	struct peer *p = link(conn);
	p->conn = conn;
	p->temporary_bond = !known;
	err = wait_for(p, CONNECTED, 6500);
#ifdef CONFIG_USB_ASHA_POWER_BUTTON
	if (!err) { err = radio_set_link_power(conn, atomic_get(&selected_dbm)); }
#endif
	if (!err) { err = setup(p); }
	if (err) { drop(p, err); }
	else {
		indicator_confirm();
		tell_other(p, 1);
		if (!(p->props[1] & 2) || asha_ready() == SIDES) { atomic_clear(&pairing); }
	}
}

void asha_poll(void)
{
	if (k_uptime_get() >= pair_until) { atomic_clear(&pairing); }
	unsigned occupied = 0;
	for (unsigned i = 0; i < SIDES; ++i) {
		struct peer *p = &peers[i];
		if (!p->conn) { continue; }
		uint32_t events = k_event_test(&p->events, DOWN | FAILED | TIMING);
		if (events & DOWN) {
			bool was_ready = p->ready || p->started;
			p->started = p->ready = false;
			if (was_ready || playing) {
				asha_stop();
				tell_other(p, 0);
			}
			if (p->temporary_bond) { bt_unpair(BT_ID_DEFAULT, bt_conn_get_dst(p->conn)); }
			struct bt_conn *old = p->conn;
			memset(p, 0, sizeof(*p));
			k_event_init(&p->events);
			bt_conn_unref(old);
			scan_after = k_uptime_get() + 1000;
			continue;
		}
		if ((events & FAILED) && p->ready) {
			drop(p, -EIO);
			asha_stop();
			tell_other(p, 0);
		}
		if (events & TIMING) {
			k_event_clear(&p->events, TIMING);
			tell_other(p, 2);
		}
		occupied++;
	}
	unsigned ready = asha_ready();
	atomic_set(&diag_links, ready);
	if (occupied >= SIDES || (ready && !(peers[peers[0].ready ? 0 : 1].props[1] & 2))) {
		stop_scan();
		return;
	}
	if (k_uptime_get() < scan_after) { return; }
	if (!scanning) {
		const struct bt_le_scan_param scan = {.type = BT_LE_SCAN_TYPE_ACTIVE, .interval = 96, .window = 32};
		scanning = bt_le_scan_start(&scan, found) == 0;
		if (!scanning) { scan_after = k_uptime_get() + 500; }
	}
	bt_addr_le_t addr;
	if (k_msgq_get(&candidates, &addr, K_NO_WAIT) == 0) { attach(&addr); }
}

#ifdef CONFIG_USB_ASHA_POWER_BUTTON
int asha_power_dbm(void)
{
	return atomic_get(&selected_dbm);
}

/* Peer setup, removal and button presses run on the same application thread. */
void asha_cycle_power(void)
{
	int prior = asha_power_dbm();
	int next = next_radio_dbm(prior);
	unsigned touched = 0;
	int err = 0;
	for (unsigned i = 0; i < SIDES; ++i) {
		struct peer *p = &peers[i];
		if (!p->conn || k_event_test(&p->events, DOWN | FAILED)) { continue; }
		touched |= BIT(i);
		err = radio_set_link_power(p->conn, next);
		if (err) { break; }
	}
	if (!err) { err = radio_set_scan_power(next); }
	if (!err) {
		atomic_set(&selected_dbm, next);
		indicator_feedback(1 + next / 4);
		return;
	}

	/* Keep both live links on the last confirmed level after a partial write. */
	for (unsigned i = 0; i < SIDES; ++i) {
		if ((touched & BIT(i)) && radio_set_link_power(peers[i].conn, prior)) {
			drop(&peers[i], err);
		}
	}
	atomic_set(&diag_error, err);
	indicator_feedback(0);
}
#endif

void asha_pair(void)
{
	atomic_set(&pairing, 1);
	indicator_set(LED_SEARCH);
	pair_until = k_uptime_get() + 60000;
	scan_after = 0;
}
#ifdef CONFIG_USB_ASHA_SONNET3
void asha_toggle_mics(void)
{
	struct bt_conn *connections[SIDES] = {0};
	for (unsigned i = 0; i < SIDES; ++i) {
		if (peers[i].ready) { connections[i] = peers[i].conn; }
	}
	sonnet_toggle(connections);
}
#endif

int asha_forget(void)
{
#ifdef CONFIG_USB_ASHA_SONNET3
	sonnet_cancel();
#endif
	stop_scan();
	atomic_clear(&pairing);
	asha_stop();
	for (unsigned i = 0; i < SIDES; ++i) {
		if (peers[i].conn) { drop(&peers[i], 0); }
	}
	return bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
}
int asha_init(void)
{
	for (unsigned i = 0; i < SIDES; ++i) { k_event_init(&peers[i].events); }
	int err = bt_enable(NULL);
	return err ? err : settings_load();
}

static bool start(unsigned count)
{
	for (unsigned i = 0; i < SIDES; ++i) {
		struct peer *p = &peers[i];
		if (!p->ready) { continue; }
		int err = audio_reset(i) ? control(p, true, count == SIDES) : -ENOMEM;
		if (err) {
			drop(p, err);
			asha_stop();
			return false;
		}
		p->started = true;
	}
	stream++;
	sequence = 0;
	sent_first_frame = false;
	playing = true;
	atomic_set(&diag_stage, STREAMING);
	audio_flush();
	return true;
}

static void release_unsent(struct net_buf *buffers[SIDES])
{
	for (unsigned i = 0; i < SIDES; ++i) {
		if (buffers[i]) { net_buf_unref(buffers[i]); }
	}
}

void asha_audio(const struct audio_frame *frame)
{
	unsigned count = asha_ready();
	if (!count) { return; }
	if (!playing) {
		start(count);
		return; /* Consume a fresh frame after Start acknowledgements. */
	}
	struct net_buf *buffers[SIDES] = {0};
	for (unsigned i = 0; i < SIDES; ++i) {
		if (!peers[i].started) { continue; }
		buffers[i] = net_buf_alloc(&audio_buffers, K_NO_WAIT);
		if (!buffers[i]) {
			atomic_inc(&diag_tx_full);
			release_unsent(buffers);
			if (sent_first_frame) { sequence++; }
			return;
		}
	}

	int err = 0;
	unsigned slot;
	int8_t level = audio_volume();
	for (slot = 0; slot < SIDES; ++slot) {
		struct net_buf *buf = buffers[slot];
		if (!buf) { continue; }
		struct peer *p = &peers[slot];
		if (level != p->volume &&
		    !bt_gatt_write_without_response(p->conn, p->handle[VOLUME], &level, sizeof(level), false)) {
			p->volume = level;
		}
		net_buf_reserve(buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE);
		net_buf_add_u8(buf, sequence);
		enum audio_route route = count == 1 ? ROUTE_MONO :
			(p->props[1] & 1) ? ROUTE_RIGHT : ROUTE_LEFT;
		if (!audio_encode(slot, route, frame, net_buf_add(buf, CODE_BYTES))) {
			err = -EIO;
			goto failed;
		}
	}

	/* On this single-core controller, the host TX worker can run after both
	 * nonblocking L2CAP queue operations. Radio interrupts can run throughout. */
	k_sched_lock();
	for (slot = 0; slot < SIDES; ++slot) {
		if (!buffers[slot]) { continue; }
		struct peer *p = &peers[slot];
		timing_submit(slot, p->props[1] & 1, stream, sequence, frame);
		err = bt_l2cap_chan_send(&p->channel.chan, buffers[slot]);
		if (err < 0) {
			timing_reject(slot);
			break;
		}
		buffers[slot] = NULL; /* The Bluetooth stack now owns this SDU. */
		atomic_inc(&diag_asdu);
	}
	k_sched_unlock();
	if (err < 0) { goto failed; }
	sent_first_frame = true;
	sequence++;
	return;

failed:
	release_unsent(buffers);
	drop(&peers[slot], err);
	asha_stop();
	tell_other(&peers[slot], 0);
}
