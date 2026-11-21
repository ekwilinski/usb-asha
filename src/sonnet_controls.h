#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The processor stores one streaming source mask per listening program. */
enum sonnet_mode { MIC_UNKNOWN, MIC_MIXED, MIC_STREAM_ONLY };
static inline enum sonnet_mode sonnet_current(const uint8_t flags[5], uint8_t source, uint8_t program)
{
	if (source >= 8 || program >= 5) { return MIC_UNKNOWN; }
	uint8_t mask = flags[program];
	if (!mask) { return MIC_MIXED; }
	if ((mask & 0xfe) == 0xfe) { return MIC_STREAM_ONLY; }
	if (!source) { return MIC_UNKNOWN; }
	return (mask & (1U << source)) ? MIC_STREAM_ONLY : MIC_MIXED;
}
