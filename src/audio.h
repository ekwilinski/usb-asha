#pragma once

#include <zephyr/kernel.h>
#include <stdint.h>

enum { SIDES = 2, USB_SAMPLES = 960, PCM_SAMPLES = 320, CODE_BYTES = 160 };
struct audio_frame {
	uint32_t time_ms, number, usb_tick;
	int16_t samples[SIDES][USB_SAMPLES];
};

enum audio_route { ROUTE_LEFT, ROUTE_RIGHT, ROUTE_MONO };
int audio_init(void);
struct audio_frame *audio_take(k_timeout_t timeout);
void audio_release(struct audio_frame *frame);
void audio_flush(void);
int8_t audio_volume(void);
bool audio_reset(unsigned slot);
bool audio_encode(unsigned slot, enum audio_route route, const struct audio_frame *frame,
		  uint8_t out[CODE_BYTES]);

/* Small read-only status snapshot for the USB host. */
extern atomic_t diag_usb_packets, diag_frames, diag_asdu, diag_links, diag_stage, diag_error;
extern atomic_t diag_advertisements, diag_matches, diag_disconnects;
extern atomic_t diag_pcm_overruns, diag_pcm_stale, diag_tx_full, diag_encode_us, diag_encode_peak_us;
