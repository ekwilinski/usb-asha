#include "../src/sonnet_controls.h"
#include <assert.h>

int main(void)
{
	/* Match the old curve for every low master and per-channel setting. */
	for (int master = -48; master <= 0; ++master) {
		int previous = 0;
		for (int channel = -48; channel <= 0; ++channel) {
			int boost = sonnet_boost_steps(master, channel, 0);
			assert(boost == sonnet_boost_steps(master, 0, channel));
			if (master + channel <= -24) { assert(boost == 0); }
			assert(boost >= previous && boost <= 16);
			previous = boost;
		}
	}
	assert(sonnet_boost_steps(-12, 0, 0) == 8);
	assert(sonnet_boost_steps(0, 0, 0) == 16);

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
		struct sonnet_button b = {0};
		assert(sonnet_press(&b, true, 100) == BUTTON_NONE);
		enum sonnet_press expected = duration >= 5000 ? BUTTON_RESET :
			duration >= 30 && duration < 750 ? BUTTON_TAP : BUTTON_NONE;
		assert(sonnet_press(&b, false, 100 + duration) == expected);
		assert(sonnet_press(&b, false, 6000) == BUTTON_NONE);
	}
	struct sonnet_button b = {0};
	assert(sonnet_press(&b, true, 0) == BUTTON_NONE);
	assert(sonnet_press(&b, true, 4999) == BUTTON_NONE);
	assert(sonnet_press(&b, true, 5000) == BUTTON_RESET);
	assert(sonnet_press(&b, true, 6000) == BUTTON_NONE);
	assert(sonnet_press(&b, false, 6500) == BUTTON_NONE);
	assert(sonnet_press(&b, true, 7000) == BUTTON_NONE);
	assert(sonnet_press(&b, false, 7100) == BUTTON_TAP);
}
