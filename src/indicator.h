#pragma once
#include <stdbool.h>

enum led_state { LED_IDLE, LED_SEARCH, LED_CONNECT, LED_CONNECTED, LED_AUDIO, LED_ERROR };
void indicator_init(void);
void indicator_set(enum led_state state);
void indicator_confirm(void);
bool indicator_on(void);
#ifdef CONFIG_USB_ASHA_SONNET3
void indicator_mix(unsigned flashes);
#endif
