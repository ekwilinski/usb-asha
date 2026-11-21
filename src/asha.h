#pragma once
#include "audio.h"

int asha_init(void);
void asha_pair(void);
#ifdef CONFIG_USB_ASHA_POWER_BUTTON
int asha_power_dbm(void);
void asha_cycle_power(void);
#endif
int asha_forget(void);
#ifdef CONFIG_USB_ASHA_SONNET3
void asha_toggle_mics(void);
#endif
bool asha_pairing(void);
unsigned asha_ready(void);
bool asha_playing(void);
void asha_poll(void);
void asha_audio(const struct audio_frame *frame);
void asha_stop(void);
