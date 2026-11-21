#include "audio.h"
#ifdef CONFIG_USB_ASHA_SONNET3
#include "sonnet_controls.h"
#include "sonnet_mix.h"
#endif
#include "asha.h"
#include "indicator.h"
#include "packet_timing.h"
#include <zephyr/fatal.h>
#include <zephyr/sys/reboot.h>
#include "g722_encoder.h"
#include <dsp/filtering_functions.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_audio.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

atomic_t diag_usb_packets, diag_frames, diag_asdu, diag_links, diag_stage, diag_error;
atomic_t diag_advertisements, diag_matches, diag_disconnects;
atomic_t diag_pcm_overruns, diag_pcm_stale, diag_tx_full, diag_encode_us, diag_encode_peak_us;
static __noinit struct { uint32_t magic, reason, pc, stage; } last_fault;
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	last_fault.reason = reason;
	last_fault.pc = esf ? esf->basic.pc : 0;
	last_fault.stage = atomic_get(&diag_stage);
	last_fault.magic = 0x41534842;
	sys_reboot(SYS_REBOOT_WARM);
}
static const struct device *const hp = DEVICE_DT_GET_ONE(usb_audio_hp);
/* Producer, consumer and two queued frames each own a separate slab block. */
K_MEM_SLAB_DEFINE(frame_pool, sizeof(struct audio_frame), 4, 4);
K_MSGQ_DEFINE(frames, sizeof(struct audio_frame *), 2, sizeof(void *));
static struct audio_frame *assembling;
static unsigned filled;
static uint32_t previous_packet;
static atomic_t mute[3], db[3];

/* 48 kHz, 7.2 kHz cutoff, 60-tap Kaiser window (beta 6), Q15. */
static const q15_t taps[] = {
	2,9,10,-3,-27,-36,-8,50,88,51,-63,-169,-147,39,267,318,66,-353,-582,-315,
	373,961,822,-219,-1535,-1937,-448,2892,6818,9461,9461,6818,2892,-448,-1937,-1535,
	-219,822,961,373,-315,-582,-353,66,318,267,39,-147,-169,-63,51,88,50,-8,-36,-27,-3,10,9,2
};
static struct {
	G722_ENC_CTX *encoder;
	arm_fir_decimate_instance_q15 fir;
	q15_t history[USB_SAMPLES + ARRAY_SIZE(taps) - 1];
} paths[SIDES];
static int16_t mixed[USB_SAMPLES], filtered[PCM_SAMPLES];

/* Per-channel USB attenuation; the master level goes to the ASHA Volume characteristic. */
static const uint16_t gain_q15[] = {
	32768,29205,26029,23198,20675,18427,16423,14637,13045,11627,10362,9235,
	8231,7336,6538,5827,5193,4629,4125,3677,3277,2920,2603,2320,2068,1843,
	1642,1464,1305,1163,1036,924,823,734,654,583,519,463,413,368,328,292,
	260,232,207,184,164,146,130
};

void audio_release(struct audio_frame *frame)
{
	k_mem_slab_free(&frame_pool, frame);
}

static void publish_frame(uint32_t now)
{
	assembling->time_ms = now;
	assembling->usb_tick = k_cycle_get_32();
	assembling->number = (uint32_t)atomic_inc(&diag_frames) + 1;
	if (k_msgq_put(&frames, &assembling, K_NO_WAIT) != 0) {
		struct audio_frame *old;
		atomic_inc(&diag_pcm_overruns);
		if (!k_msgq_get(&frames, &old, K_NO_WAIT)) { audio_release(old); }
		if (k_msgq_put(&frames, &assembling, K_NO_WAIT) != 0) { audio_release(assembling); }
	}
	assembling = NULL;
	filled = 0;
}

static void receive(const struct device *dev, struct net_buf *buf, size_t bytes)
{
	uint32_t now = k_uptime_get_32();
	if (now - previous_packet > 8) {
		filled = 0;
	}
	previous_packet = now;
	if (bytes && bytes <= 192 && bytes % 4 == 0) {
		atomic_inc(&diag_usb_packets);
		for (size_t i = 0; i < bytes; i += 4) {
			if (!assembling && k_mem_slab_alloc(&frame_pool, (void **)&assembling, K_NO_WAIT)) {
				break;
			}
			assembling->samples[0][filled] = (int16_t)sys_get_le16(buf->data + i);
			assembling->samples[1][filled++] = (int16_t)sys_get_le16(buf->data + i + 2);
			if (filled == USB_SAMPLES) {
				publish_frame(now);
			}
		}
	}
	net_buf_unref(buf);
}

static void feature(const struct device *dev, const struct usb_audio_fu_evt *evt)
{
	if (evt->channel >= ARRAY_SIZE(db)) {
		return;
	}
	if (evt->cs == USB_AUDIO_FU_MUTE_CONTROL && evt->val_len == 1) {
		atomic_set(&mute[evt->channel], *(const uint8_t *)evt->val != 0);
	} else if (evt->cs == USB_AUDIO_FU_VOLUME_CONTROL && evt->val_len == 2) {
		int level = (int16_t)sys_get_le16(evt->val) / 256;
		atomic_set(&db[evt->channel], CLAMP(level, -48, 0));
	}
}

int8_t audio_volume(void)
{
	if (atomic_get(&mute[0])) { return -128; }
	int master = atomic_get(&db[0]);
	int steps = (master - 18) * 8 / 3;
#ifdef CONFIG_USB_ASHA_SONNET3
	steps += sonnet_boost_steps(master, atomic_get(&db[1]), atomic_get(&db[2]));
#endif
	return MAX(-127, steps);
}

static int read_status(struct usb_setup_packet *req, int32_t *len, uint8_t **data)
{
	if (req->bRequest == 0x43 || req->bRequest == 0x44) { return timing_read(req, len, data); }
	if (req->bmRequestType != 0xc0 || req->wValue || req->wIndex) { return -ENOTSUP; }
#ifdef CONFIG_USB_ASHA_SONNET3
	if (req->bRequest == 0x45 && req->wLength == 4) {
		static uint32_t mix;
		mix = sys_cpu_to_le32(sonnet_status());
		*data = (uint8_t *)&mix;
		*len = sizeof(mix);
		return 0;
	}
#endif
	if (req->bRequest == 0x42 && req->wLength == 20) {
		static uint32_t timing[5];
		timing[0] = sys_cpu_to_le32(atomic_get(&diag_pcm_overruns));
		timing[1] = sys_cpu_to_le32(atomic_get(&diag_pcm_stale));
		timing[2] = sys_cpu_to_le32(atomic_get(&diag_tx_full));
		timing[3] = sys_cpu_to_le32(atomic_get(&diag_encode_us));
		timing[4] = sys_cpu_to_le32(atomic_get(&diag_encode_peak_us));
		*data = (uint8_t *)timing;
		*len = sizeof(timing);
		return 0;
	}
	if (req->bRequest != 0x41 || req->wLength != 64) {
		return -ENOTSUP;
	}
	static uint32_t report[16];
	report[0] = sys_cpu_to_le32(3);
	report[1] = sys_cpu_to_le32(atomic_get(&diag_usb_packets));
	report[2] = sys_cpu_to_le32(atomic_get(&diag_frames));
	report[3] = sys_cpu_to_le32(atomic_get(&diag_links));
	report[4] = sys_cpu_to_le32(atomic_get(&diag_asdu));
	report[5] = sys_cpu_to_le32(atomic_get(&diag_stage));
	report[6] = sys_cpu_to_le32(atomic_get(&diag_error));
	report[7] = sys_cpu_to_le32(atomic_get(&diag_advertisements));
	report[8] = sys_cpu_to_le32(atomic_get(&diag_matches));
	report[9] = sys_cpu_to_le32(asha_pairing());
	report[10] = sys_cpu_to_le32(indicator_on());
	report[11] = sys_cpu_to_le32(k_uptime_get_32());
	bool fault = last_fault.magic == 0x41534842;
	report[12] = sys_cpu_to_le32(fault ? last_fault.reason + 1 : 0);
	report[13] = sys_cpu_to_le32(fault ? last_fault.pc : 0);
	report[14] = sys_cpu_to_le32(fault ? last_fault.stage : 0);
	report[15] = sys_cpu_to_le32(atomic_get(&diag_disconnects));
	*data = (uint8_t *)report;
	*len = sizeof(report);
	return 0;
}

int audio_init(void)
{
	static const struct usb_audio_ops ops = {.data_received_cb = receive, .feature_update_cb = feature};
	if (!device_is_ready(hp)) {
		return -ENODEV;
	}
	/* Zephyr allocates the per-interface USB configuration in RAM. */
	struct usb_cfg_data *cfg = (struct usb_cfg_data *)hp->config;
	cfg->interface.vendor_handler = read_status;
	usb_audio_register(hp, &ops);
	return usb_enable(NULL);
}

struct audio_frame *audio_take(k_timeout_t timeout)
{
	struct audio_frame *frame;
	if (k_msgq_get(&frames, &frame, timeout) != 0) { return NULL; }
	if (k_uptime_get_32() - frame->time_ms < 45) { return frame; }
	atomic_inc(&diag_pcm_stale);
	audio_release(frame);
	return NULL;
}

void audio_flush(void)
{
	struct audio_frame *frame;
	while (!k_msgq_get(&frames, &frame, K_NO_WAIT)) { audio_release(frame); }
}

bool audio_reset(unsigned slot)
{
	if (paths[slot].encoder) {
		g722_encoder_destroy(paths[slot].encoder);
	}
	paths[slot].encoder = g722_encoder_new(64000, G722_DEFAULT);
	arm_status status = arm_fir_decimate_init_q15(&paths[slot].fir, ARRAY_SIZE(taps), 3,
						     taps, paths[slot].history, USB_SAMPLES);
	return status == ARM_MATH_SUCCESS && paths[slot].encoder;
}

bool audio_encode(unsigned slot, enum audio_route route, const struct audio_frame *frame,
		  uint8_t out[CODE_BYTES])
{
	int64_t began = k_uptime_ticks();
	int32_t left = atomic_get(&mute[1]) ? 0 : gain_q15[-atomic_get(&db[1])];
	int32_t right = atomic_get(&mute[2]) ? 0 : gain_q15[-atomic_get(&db[2])];
	for (unsigned i = 0; i < USB_SAMPLES; ++i) {
		int32_t l = (frame->samples[0][i] * left) >> 15;
		int32_t r = (frame->samples[1][i] * right) >> 15;
		mixed[i] = route == ROUTE_MONO ? (l + r) / 2 : route == ROUTE_LEFT ? l : r;
	}
	arm_fir_decimate_q15(&paths[slot].fir, mixed, filtered, USB_SAMPLES);
	int length = g722_encode(paths[slot].encoder, filtered, PCM_SAMPLES, out);
	uint32_t us = k_ticks_to_us_floor32(k_uptime_ticks() - began);
	atomic_set(&diag_encode_us, us);
	if (us > atomic_get(&diag_encode_peak_us)) { atomic_set(&diag_encode_peak_us, us); }
	return length == CODE_BYTES;
}
