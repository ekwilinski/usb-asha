#pragma once
#include <zephyr/bluetooth/conn.h>

int radio_set_scan_power(int8_t dbm);
int radio_set_link_power(struct bt_conn *conn, int8_t dbm);
