#include "asha.h"
#include "sonnet_controls.h"
#include "indicator.h"
#include <zephyr/drivers/gpio.h>

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

int main(void)
{
	indicator_init();
	gpio_pin_configure_dt(&button, GPIO_INPUT);
	int err = audio_init();
	if (!err) { err = asha_init(); }
	if (err) {
		atomic_set(&diag_error, err);
		indicator_set(LED_ERROR);
		return err;
	}
	int64_t last_audio = 0;
#ifdef CONFIG_USB_ASHA_SONNET3
	struct sonnet_button button_state = {0};
#else
	int64_t pressed = -1;
#endif
	for (;;) {
		int64_t now = k_uptime_get();
		bool down = gpio_pin_get_dt(&button) > 0;
#ifdef CONFIG_USB_ASHA_SONNET3
		enum sonnet_press action = sonnet_press(&button_state, down, now);
		if (action == BUTTON_RESET) {
			if (!asha_forget()) { asha_pair(); }
			else { indicator_mix(0); }
		} else if (action == BUTTON_TAP) {
			asha_toggle_mics();
		}
#else
		if (down) {
			if (pressed < 0) { pressed = now; }
		} else if (pressed >= 0) {
			if (now - pressed >= 5000) { asha_forget(); }
			else if (now - pressed >= 30) { asha_pair(); }
			pressed = -1;
		}
#endif
		asha_poll();
		struct audio_frame *frame = audio_take(K_MSEC(10));
		if (frame) {
			last_audio = k_uptime_get();
			asha_audio(frame);
			audio_release(frame);
		} else if (asha_playing() && k_uptime_get() - last_audio >= 150) {
			asha_stop();
		}
		enum led_state state = asha_playing() ? LED_AUDIO : asha_ready() ? LED_CONNECTED :
				       asha_pairing() ? LED_SEARCH : LED_IDLE;
		indicator_set(state);
	}
}
