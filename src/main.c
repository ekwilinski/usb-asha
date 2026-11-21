#include "asha.h"
#include "dongle_controls.h"
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
#if defined(CONFIG_USB_ASHA_SONNET3) || defined(CONFIG_USB_ASHA_POWER_BUTTON)
	struct button_state button_state = {0};
#else
	int64_t pressed = -1;
#endif
	for (;;) {
		int64_t now = k_uptime_get();
		bool down = gpio_pin_get_dt(&button) > 0;
#if defined(CONFIG_USB_ASHA_SONNET3) || defined(CONFIG_USB_ASHA_POWER_BUTTON)
		enum button_action action = button_update(&button_state, down, now);
#ifdef CONFIG_USB_ASHA_POWER_BUTTON
		if (action == BUTTON_POWER) { asha_cycle_power(); }
#endif
		if (action == BUTTON_RESET) {
#ifdef CONFIG_USB_ASHA_SONNET3
			if (!asha_forget()) { asha_pair(); }
			else { indicator_feedback(0); }
#else
			if (asha_forget()) { indicator_feedback(0); }
#endif
		} else if (action == BUTTON_TAP) {
#ifdef CONFIG_USB_ASHA_SONNET3
			asha_toggle_mics();
#else
			asha_pair();
#endif
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
