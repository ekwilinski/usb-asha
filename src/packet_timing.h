#pragma once

#include "audio.h"
#include <zephyr/usb/usb_device.h>

void timing_channel_open(unsigned slot);
void timing_submit(unsigned slot, unsigned ear, uint16_t stream, uint8_t sequence,
		   const struct audio_frame *frame);
void timing_reject(unsigned slot);
void timing_complete(unsigned slot);
int timing_read(const struct usb_setup_packet *req, int32_t *len, uint8_t **data);
