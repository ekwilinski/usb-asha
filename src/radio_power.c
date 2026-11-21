#include "radio_power.h"
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/sys/byteorder.h>
#include <errno.h>

static int set_power(uint8_t type, uint16_t handle, int8_t dbm)
{
	struct net_buf *buf = bt_hci_cmd_alloc(K_NO_WAIT);
	if (!buf) { return -ENOBUFS; }
	struct bt_hci_cp_vs_write_tx_power_level *cmd = net_buf_add(buf, sizeof(*cmd));
	cmd->handle_type = type;
	cmd->handle = sys_cpu_to_le16(handle);
	cmd->tx_power_level = dbm;

	struct net_buf *rsp = NULL;
	int err = bt_hci_cmd_send_sync(BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL, buf, &rsp);
	if (err) { return err; }
	const struct bt_hci_rp_vs_write_tx_power_level *result = (const void *)rsp->data;
	if (rsp->len != sizeof(*result) || result->status || result->handle_type != type ||
	    sys_le16_to_cpu(result->handle) != handle || result->selected_tx_power != dbm) {
		err = -EIO;
	}
	net_buf_unref(rsp);
	return err;
}

int radio_set_scan_power(int8_t dbm)
{
	return set_power(BT_HCI_VS_LL_HANDLE_TYPE_SCAN, 0, dbm);
}

int radio_set_link_power(struct bt_conn *conn, int8_t dbm)
{
	uint16_t handle;
	int err = bt_hci_get_conn_handle(conn, &handle);
	return err ? err : set_power(BT_HCI_VS_LL_HANDLE_TYPE_CONN, handle, dbm);
}
