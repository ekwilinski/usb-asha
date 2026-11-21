#include "../src/sonnet_controls.h"
#include "../src/dongle_controls.h"
#include <assert.h>

int main(void)
{
	/* Match the old curve for every low master and per-channel setting. */
	for (int master = -48; master <= 0; ++master) {
		int previous = 0;
		for (int channel = -48; channel <= 0; ++channel) {
			int boost = stream_boost_steps(master, channel, 0);
			assert(boost == stream_boost_steps(master, 0, channel));
			if (master + channel <= -24) { assert(boost == 0); }
			assert(boost >= previous && boost <= 16);
			previous = boost;
		}
	}
	assert(stream_boost_steps(-12, 0, 0) == 8);
	assert(stream_boost_steps(0, 0, 0) == 16);

	const uint8_t flags[] = {0, 0xff, 0xfe, 0x02, 0x80};
	assert(sonnet_current(flags, 0, 0) == MIC_MIXED);
	assert(sonnet_current(flags, 0, 1) == MIC_STREAM_ONLY);
	assert(sonnet_current(flags, 5, 2) == MIC_STREAM_ONLY);
	assert(sonnet_current(flags, 0, 3) == MIC_UNKNOWN);
	assert(sonnet_current(flags, 1, 3) == MIC_STREAM_ONLY);
	assert(sonnet_current(flags, 5, 3) == MIC_MIXED);
	assert(sonnet_current(flags, 7, 4) == MIC_STREAM_ONLY);
	assert(sonnet_current(flags, 8, 0) == MIC_UNKNOWN);
	assert(sonnet_current(flags, 0, 5) == MIC_UNKNOWN);

	for (int duration = 0; duration <= 5100; ++duration) {
		struct button_state b = {0};
		assert(button_update(&b, true, 100) == BUTTON_NONE);
		enum button_action expected = duration >= 5000 ? BUTTON_RESET :
			duration >= 30 && duration < 750 ? BUTTON_TAP : BUTTON_NONE;
#ifdef CONFIG_USB_ASHA_POWER_BUTTON
		if (duration >= 2000 && duration < 5000) { expected = BUTTON_POWER; }
#endif
		assert(button_update(&b, true, 100 + (duration < 5000 ? duration : 4999)) == BUTTON_NONE);
		assert(button_update(&b, false, 100 + duration) == expected);
		assert(button_update(&b, false, 6000) == BUTTON_NONE);
	}
	assert(next_radio_dbm(4) == 8 && next_radio_dbm(8) == 0 && next_radio_dbm(0) == 4);
	struct button_state b = {0};
	assert(button_update(&b, true, 0) == BUTTON_NONE);
	assert(button_update(&b, true, 4999) == BUTTON_NONE);
	assert(button_update(&b, true, 5000) == BUTTON_RESET);
	assert(button_update(&b, true, 6000) == BUTTON_NONE);
	assert(button_update(&b, false, 6500) == BUTTON_NONE);
	assert(button_update(&b, true, 7000) == BUTTON_NONE);
	assert(button_update(&b, false, 7100) == BUTTON_TAP);
}
