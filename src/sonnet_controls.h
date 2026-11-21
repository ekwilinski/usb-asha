#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The processor stores one streaming source mask per listening program. */
enum sonnet_mode { MIC_UNKNOWN, MIC_MIXED, MIC_STREAM_ONLY };
static inline enum sonnet_mode sonnet_current(const uint8_t flags[5], uint8_t source, uint8_t program)
{
	if (source >= 8 || program >= 5) { return MIC_UNKNOWN; }
	uint8_t mask = flags[program];
	if (!mask) { return MIC_MIXED; }
	if ((mask & 0xfe) == 0xfe) { return MIC_STREAM_ONLY; }
	if (!source) { return MIC_UNKNOWN; }
	return (mask & (1U << source)) ? MIC_STREAM_ONLY : MIC_MIXED;
}

static inline int sonnet_boost_steps(int master_db, int left_db, int right_db)
{
	/* 0.375 dB per ASHA step. Add nothing through -24 dB USB. */
	int level = master_db + (left_db < right_db ? left_db : right_db);
	return level > -24 ? (level + 24) * 16 / 24 : 0;
}

enum sonnet_press { BUTTON_NONE, BUTTON_TAP, BUTTON_RESET };
struct sonnet_button { int64_t since; bool down, handled; };
static inline enum sonnet_press sonnet_press(struct sonnet_button *b, bool down, int64_t now)
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
	return was_down && !down && !b->handled && held >= 30 && held < 750 ? BUTTON_TAP : BUTTON_NONE;
}
