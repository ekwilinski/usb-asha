#pragma once
#include <stdbool.h>
#include <stdint.h>

static inline int stream_boost_steps(int master_db, int left_db, int right_db)
{
	/* 0.375 dB per ASHA step. Add nothing through -24 dB USB. */
	int level = master_db + (left_db < right_db ? left_db : right_db);
	return level > -24 ? (level + 24) * 16 / 24 : 0;
}

enum button_action { BUTTON_NONE, BUTTON_TAP, BUTTON_RESET, BUTTON_POWER };
struct button_state { int64_t since; bool down, handled; };
static inline enum button_action button_update(struct button_state *b, bool down, int64_t now)
{
	if (!b->down && down) {
		b->since = now;
		b->handled = false;
	}
	int64_t held = now - b->since;
	bool was_down = b->down;
	b->down = down;
	if (was_down && !b->handled && held >= 5000) {
		b->handled = true;
		return BUTTON_RESET;
	}
#ifdef CONFIG_USB_ASHA_POWER_BUTTON
	if (was_down && !down && !b->handled && held >= 2000) { return BUTTON_POWER; }
#endif
	return was_down && !down && !b->handled && held >= 30 && held < 750 ? BUTTON_TAP : BUTTON_NONE;
}

static inline int next_radio_dbm(int current)
{
	return current == 8 ? 0 : current + 4;
}
