#include "sonnet_mix.h"
#include "sonnet_controls.h"
#include "indicator.h"
#include <zephyr/bluetooth/gatt.h>
#include <string.h>

#define SONNET_UUID(word) BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x5f3b0000U | (word), 0x961f, 0x1883, 0xd24e, 0x59d8b0f44f78))
static const struct bt_uuid_128 service = SONNET_UUID(0x2e9a), operating = SONNET_UUID(0xe8e7);
enum { FLAGS, SOURCE, PROGRAM, NCHARS };
static const struct bt_uuid_128 chars[NCHARS] = {
	SONNET_UUID(0xd8d1), SONNET_UUID(0xd8d2), SONNET_UUID(0xd8d3)
};
struct target {
	struct bt_conn *conn;
	uint16_t handle[NCHARS];
	enum sonnet_mode mode;
};

/* A single worker owns all ATT parameters until their callbacks finish.
 * Zephyr completes outstanding requests on ATT timeout or disconnect. */
static struct target targets[SIDES];
static atomic_t busy, cancelled, report;
enum mix_result { IDLE, CHECKING, STREAM_ONLY, WITH_MIC, UNCONFIRMED, CANCELLED };
enum mix_stage { NONE, IDENTITY, SERVICE, READ_MODE, WRITE_MODE, VERIFY };
/* Last button transaction, in USB wire order. */
static void publish(enum mix_result result, enum mix_stage stage, unsigned count, unsigned confirmed)
{
	atomic_set(&report, result | (stage << 8) | (count << 16) | (confirmed << 24));
}
uint32_t sonnet_status(void) { return atomic_get(&cancelled) ? CANCELLED : atomic_get(&report); }

K_SEM_DEFINE(mix_request, 0, 1);
K_SEM_DEFINE(mix_reply, 0, 1);
static struct {
	struct bt_gatt_discover_params discovery;
	struct bt_gatt_read_params read;
	struct bt_gatt_write_params write;
	uint16_t first, last;
	struct target *target;
	uint8_t data[48];
	int length;
} op;

static uint8_t found(struct bt_conn *conn, const struct bt_gatt_attr *attr,
		     struct bt_gatt_discover_params *params)
{
	if (!attr) {
		k_sem_give(&mix_reply);
		return BT_GATT_ITER_STOP;
	}
	if (params->type == BT_GATT_DISCOVER_PRIMARY) {
		const struct bt_gatt_service_val *svc = attr->user_data;
		op.first = attr->handle + 1;
		op.last = svc->end_handle;
		k_sem_give(&mix_reply);
		return BT_GATT_ITER_STOP;
	}
	const struct bt_gatt_chrc *ch = attr->user_data;
	for (unsigned i = 0; i < NCHARS; ++i) {
		uint8_t properties = BT_GATT_CHRC_READ | (i == FLAGS ? BT_GATT_CHRC_WRITE : 0);
		if (!bt_uuid_cmp(ch->uuid, &chars[i].uuid) && (ch->properties & properties) == properties) {
			op.target->handle[i] = ch->value_handle;
		}
	}
	return BT_GATT_ITER_CONTINUE;
}

static bool discover(struct target *t, bool primary)
{
	if (atomic_get(&cancelled)) { return false; }
	op.discovery = (struct bt_gatt_discover_params){
		.uuid = primary ? &service.uuid : NULL, .func = found,
		.start_handle = primary ? BT_ATT_FIRST_ATTRIBUTE_HANDLE : op.first,
		.end_handle = primary ? BT_ATT_LAST_ATTRIBUTE_HANDLE : op.last,
		.type = primary ? BT_GATT_DISCOVER_PRIMARY : BT_GATT_DISCOVER_CHARACTERISTIC,
	};
	if (bt_gatt_discover(t->conn, &op.discovery)) { return false; }
	k_sem_take(&mix_reply, K_FOREVER);
	return primary ? op.first && op.first <= op.last :
		t->handle[FLAGS] && t->handle[SOURCE] && t->handle[PROGRAM];
}

static uint8_t received(struct bt_conn *conn, uint8_t err, struct bt_gatt_read_params *params,
			const void *data, uint16_t len)
{
	op.length = -1;
	if (!err && data && len && len <= sizeof(op.data)) {
		memcpy(op.data, data, len);
		op.length = len;
	}
	k_sem_give(&mix_reply);
	return BT_GATT_ITER_STOP;
}

static int read_value(struct target *t, uint16_t handle, const struct bt_uuid *uuid)
{
	if (atomic_get(&cancelled)) { return -1; }
	op.read = (struct bt_gatt_read_params){.func = received, .handle_count = uuid ? 0 : 1};
	if (uuid) {
		op.read.by_uuid.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
		op.read.by_uuid.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
		op.read.by_uuid.uuid = uuid;
	} else {
		op.read.single.handle = handle;
	}
	if (bt_gatt_read(t->conn, &op.read)) { return -1; }
	k_sem_take(&mix_reply, K_FOREVER);
	return op.length;
}

static bool identify(struct target *t)
{
	if (bt_conn_get_security(t->conn) < BT_SECURITY_L2) { return false; }
	int len = read_value(t, 0, BT_UUID_DIS_MANUFACTURER_NAME);
	while (len > 0 && op.data[len - 1] <= ' ') { --len; }
	if (len != 6 || memcmp(op.data, "MED-EL", 6)) { return false; }
	if (read_value(t, 0, BT_UUID_DIS_MODEL_NUMBER) < 8 || memcmp(op.data, "SONNET 3", 8)) {
		return false;
	}
	return read_value(t, 0, &operating.uuid) > 0 && op.data[0] == 0;
}

static bool find_controls(struct target *t)
{
	op.first = op.last = 0;
	op.target = t;
	return discover(t, true) && discover(t, false);
}

static bool settings(struct target *t, uint8_t flags[5])
{
	if (read_value(t, t->handle[FLAGS], NULL) != 5) { return false; }
	memcpy(flags, op.data, 5);
	if (read_value(t, t->handle[SOURCE], NULL) != 1) { return false; }
	uint8_t source = op.data[0];
	if (read_value(t, t->handle[PROGRAM], NULL) != 1) { return false; }
	t->mode = sonnet_current(flags, source, op.data[0]);
	return t->mode != MIC_UNKNOWN;
}

static void written(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *params)
{
	op.length = err ? -1 : 0;
	k_sem_give(&mix_reply);
}

static void toggle_all(void)
{
	uint8_t flags[5];
	bool all_stream_only = true;
	unsigned count = 0, confirmed = 0;
	enum mix_stage stage = NONE;
	for (unsigned i = 0; i < SIDES; ++i) { count += targets[i].conn != NULL; }
	publish(CHECKING, NONE, count, 0);
	for (unsigned i = 0; i < SIDES; ++i) {
		struct target *t = &targets[i];
		if (!t->conn) { continue; }
		stage = IDENTITY;
		if (!identify(t)) { goto failure; }
		stage = SERVICE;
		if (!find_controls(t)) { goto failure; }
		stage = READ_MODE;
		if (!settings(t, flags)) { goto failure; }
		all_stream_only &= t->mode == MIC_STREAM_ONLY;
	}
	uint8_t request[6] = {0};
	if (!all_stream_only) { memset(request, 0xff, 5); }
	for (unsigned i = 0; i < SIDES; ++i) {
		struct target *t = &targets[i];
		if (!t->conn) { continue; }
		if (atomic_get(&cancelled)) { return; }
		stage = WRITE_MODE;
		op.write = (struct bt_gatt_write_params){
			.func = written, .handle = t->handle[FLAGS], .data = request, .length = sizeof(request)
		};
		if (bt_gatt_write(t->conn, &op.write)) { goto failure; }
		k_sem_take(&mix_reply, K_FOREVER);
		if (op.length) { goto failure; }
		stage = VERIFY;
		if (!settings(t, flags) || memcmp(flags, request, 5)) { goto failure; }
		++confirmed;
	}
	if (!atomic_get(&cancelled)) {
		publish(all_stream_only ? WITH_MIC : STREAM_ONLY, VERIFY, count, confirmed);
		indicator_feedback(all_stream_only ? 2 : 1);
	}
	return;

failure:
	if (!atomic_get(&cancelled)) {
		publish(UNCONFIRMED, stage, count, confirmed);
		indicator_feedback(0);
	}
}

static void mix_worker(void *a, void *b, void *c)
{
	for (;;) {
		k_sem_take(&mix_request, K_FOREVER);
		toggle_all();
		for (unsigned i = 0; i < SIDES; ++i) {
			if (targets[i].conn) { bt_conn_unref(targets[i].conn); }
		}
		atomic_clear(&busy);
	}
}
K_THREAD_DEFINE(sonnet_worker, 2304, mix_worker, NULL, NULL, NULL, 5, 0, 0);

void sonnet_toggle(struct bt_conn *const connections[SIDES])
{
	if (!atomic_cas(&busy, 0, 1)) { return; }
	atomic_clear(&cancelled);
	if (!connections[0] && !connections[1]) {
		publish(UNCONFIRMED, NONE, 0, 0);
		indicator_feedback(0);
		atomic_clear(&busy);
		return;
	}
	for (unsigned i = 0; i < SIDES; ++i) {
		targets[i] = (struct target){.conn = connections[i] ? bt_conn_ref(connections[i]) : NULL};
	}
	k_sem_give(&mix_request);
}
void sonnet_cancel(void) { atomic_set(&cancelled, 1); }
