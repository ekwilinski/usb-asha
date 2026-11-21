#include "indicator.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

/* Standard CX-40 assemblies populate D1 (active-low P0.06). */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_NODELABEL(led0_d1), gpios);
static atomic_t mode, confirmed, confirm_time, lit;

#ifdef CONFIG_USB_ASHA_SONNET3
static atomic_t mix_signal, mix_time;
void indicator_mix(unsigned flashes)
{
	atomic_set(&mix_time, (atomic_val_t)k_uptime_get_32());
	atomic_set(&mix_signal, flashes + 1);
}
#endif

static bool pattern(uint32_t now)
{
	if (now < 900) { return true; }
#ifdef CONFIG_USB_ASHA_SONNET3
	int signal = atomic_get(&mix_signal);
	uint32_t age = now - (uint32_t)atomic_get(&mix_time);
	if (signal > 0 && age < 1350) {
		if (signal == 1) { return age % 180 >= 90; }
		return age >= 160 && age < 160 + (signal - 1U) * 480 && (age - 160) % 480 < 320;
	}
#endif
	if (atomic_get(&confirmed)) {
		uint32_t elapsed = now - (uint32_t)atomic_get(&confirm_time);
		if (elapsed < 2400) { return elapsed % 800 < 500; }
	}
	switch (atomic_get(&mode)) {
	case LED_SEARCH:    return now % 400 < 200;
	case LED_CONNECT:   return now % 800 < 100 || (now % 800 >= 200 && now % 800 < 300);
	case LED_CONNECTED: return true;
	case LED_AUDIO:     return now % 2000 >= 100;
	case LED_ERROR:     return now % 160 < 80;
	default:            return now % 2000 < 75;
	}
}

static void tick(struct k_timer *timer)
{
	bool next = pattern(k_uptime_get_32());
	gpio_pin_set_dt(&led, next);
	atomic_set(&lit, next);
}
K_TIMER_DEFINE(indicator_timer, tick, NULL);

void indicator_init(void)
{
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	k_timer_start(&indicator_timer, K_NO_WAIT, K_MSEC(25));
}
void indicator_set(enum led_state state) { atomic_set(&mode, state); }
void indicator_confirm(void)
{
	atomic_set(&confirm_time, k_uptime_get_32());
	atomic_set(&confirmed, 1);
}
bool indicator_on(void) { return atomic_get(&lit); }
