#pragma once
#include "audio.h"
#include <zephyr/bluetooth/conn.h>
void sonnet_toggle(struct bt_conn *const connections[SIDES]);
void sonnet_cancel(void);
uint32_t sonnet_status(void);
