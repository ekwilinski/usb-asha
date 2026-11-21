#include "g722_encoder.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
	FILE *pcm = fopen("tests/fixtures/reference.pcm", "rb");
#ifdef CONFIG_USB_ASHA_SONNET3
	FILE *reference = fopen("tests/fixtures/sonnet_quality.g722", "rb");
#else
	FILE *reference = fopen("tests/fixtures/reference.g722", "rb");
#endif
	G722_ENC_CTX *encoder = g722_encoder_new(64000, G722_DEFAULT);
	if (!pcm || !reference || !encoder) { return 1; }
	for (int n = 0; n < 2; ++n) {
		uint8_t raw[640], encoded[160], expected[160];
		int16_t samples[320];
		if (fread(raw, 1, sizeof(raw), pcm) != sizeof(raw) ||
		    fread(expected, 1, sizeof(expected), reference) != sizeof(expected)) { return 1; }
		for (int i = 0; i < 320; ++i) {
			samples[i] = (int16_t)(raw[2*i] | raw[2*i+1] << 8);
		}
		if (g722_encode(encoder, samples, 320, encoded) != 160 ||
		    memcmp(encoded, expected, 160)) { return 1; }
	}
	g722_encoder_destroy(encoder);
	fclose(pcm);
	fclose(reference);
	puts("Two consecutive 20 ms ASHA payloads match the G.722 reference.");
	return 0;
}
