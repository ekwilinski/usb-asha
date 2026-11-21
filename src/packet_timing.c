#include "packet_timing.h"
#include <string.h>

/* The wire format follows the little-endian nRF52840. Times are RTC ticks. */
struct packet {
	uint32_t frame, usb_tick, submit_tick, complete_tick;
	uint16_t stream;
	uint8_t sequence, ear;
};
BUILD_ASSERT(sizeof(struct packet) == 20);

enum { PENDING = 64, HISTORY = 1024, PAGE = 32 };
static struct {
	struct { uint32_t ticket; struct packet packet; } entries[PENDING];
	uint32_t in, out;
} pending[SIDES];
static struct packet history[HISTORY];
static uint32_t next_record, retained, unmatched;
static struct k_spinlock lock;

void timing_channel_open(unsigned slot)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	/* A closed channel can discard SDUs without successful sent callbacks. */
	unmatched += pending[slot].in - pending[slot].out;
	pending[slot].in = pending[slot].out = 0;
	k_spin_unlock(&lock, key);
}

void timing_submit(unsigned slot, unsigned ear, uint16_t stream, uint8_t sequence,
		   const struct audio_frame *frame)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t ticket = pending[slot].in++;
	pending[slot].entries[ticket % PENDING].ticket = ticket;
	pending[slot].entries[ticket % PENDING].packet = (struct packet){
		.frame = frame->number, .usb_tick = frame->usb_tick,
		.submit_tick = k_cycle_get_32(), .stream = stream, .sequence = sequence, .ear = ear
	};
	k_spin_unlock(&lock, key);
}

void timing_reject(unsigned slot)
{
	/* Only the audio thread submits. A rejected send never gets a callback. */
	k_spinlock_key_t key = k_spin_lock(&lock);
	pending[slot].in--;
	k_spin_unlock(&lock, key);
}

void timing_complete(unsigned slot)
{
	uint32_t now = k_cycle_get_32();
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t outstanding = pending[slot].in - pending[slot].out;
	if (outstanding) {
		uint32_t ticket = pending[slot].out++;
		if (pending[slot].entries[ticket % PENDING].ticket == ticket) {
			struct packet *record = &history[next_record++ % HISTORY];
			*record = pending[slot].entries[ticket % PENDING].packet;
			record->complete_tick = now;
			retained = MIN(retained + 1U, HISTORY);
		} else {
			/* Its tag was overwritten. Never attribute it to a newer frame. */
			unmatched++;
		}
	} else {
		unmatched++;
	}
	k_spin_unlock(&lock, key);
}

int timing_read(const struct usb_setup_packet *req, int32_t *len, uint8_t **data)
{
	static uint32_t info[5];
	static struct timing_page {
		uint32_t first, end, unmatched;
		struct packet entries[PAGE];
	} page;
	if (req->bmRequestType != 0xc0) { return -ENOTSUP; }
	if (req->bRequest == 0x43 && !req->wIndex && !req->wValue && req->wLength == sizeof(info)) {
		k_spinlock_key_t key = k_spin_lock(&lock);
		info[0] = CONFIG_USB_ASHA_BUILD_NUMBER;
		info[1] = sys_clock_hw_cycles_per_sec();
		info[2] = next_record;
		info[3] = unmatched;
		info[4] = k_cycle_get_32();
		k_spin_unlock(&lock, key);
		*data = (uint8_t *)info;
		*len = sizeof(info);
		return 0;
	}
	if (req->bRequest != 0x44 || req->wLength != sizeof(page)) { return -ENOTSUP; }

	/* An addressed read can be safely retried after a USB error. */
	uint32_t cursor = ((uint32_t)req->wIndex << 16) | req->wValue;
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t available = MIN(next_record - cursor, retained);
	page.first = next_record - available;
	page.end = next_record;
	page.unmatched = unmatched;
	unsigned count = MIN(available, PAGE);
	unsigned pos = page.first % HISTORY;
	unsigned chunk = MIN(count, HISTORY - pos);
	memcpy(page.entries, &history[pos], chunk * sizeof(struct packet));
	memcpy(page.entries + chunk, history, (count - chunk) * sizeof(struct packet));
	k_spin_unlock(&lock, key);
	*data = (uint8_t *)&page;
	*len = offsetof(struct timing_page, entries) + count * sizeof(struct packet);
	return 0;
}
