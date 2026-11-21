#pragma once

#include <stdint.h>
#include <string.h>

/* Four neighboring decimated outputs share each coefficient load. Both
 * 72-tap halves have an absolute Q15 coefficient sum of 35505, so each
 * half fits in int32 even with full-scale input. Saturating their sum
 * preserves the final clipped Q15 result for all inputs. */
static inline int32_t fir_pair(const int16_t *samples)
{
    int32_t pair;
    __builtin_memcpy(&pair, samples, sizeof(pair));
    return pair;
}

__attribute__((noinline))
static void sonnet_decimate(const int16_t *taps, int16_t *history,
                            int16_t *output)
{
    enum { TAPS = 144, HALF = TAPS / 2 };
    for (unsigned n = 0; n < PCM_SAMPLES; n += 4) {
        const int16_t *window = history + 3 * n;
        int32_t first[4] = {0};
        for (unsigned half = 0; half < TAPS; half += HALF) {
            int32_t a = 0, b = 0, c = 0, d = 0;
            #pragma GCC unroll 6
            for (unsigned k = half; k < half + HALF; k += 2) {
                int32_t coef = fir_pair(taps + k);
                a = __builtin_arm_smlad(fir_pair(window + k), coef, a);
                b = __builtin_arm_smlad(fir_pair(window + k + 3), coef, b);
                c = __builtin_arm_smlad(fir_pair(window + k + 6), coef, c);
                d = __builtin_arm_smlad(fir_pair(window + k + 9), coef, d);
            }
            if (half == 0) {
                first[0] = a; first[1] = b; first[2] = c; first[3] = d;
            } else {
                output[n] = __builtin_arm_ssat(__builtin_arm_qadd(first[0], a) >> 15, 16);
                output[n+1] = __builtin_arm_ssat(__builtin_arm_qadd(first[1], b) >> 15, 16);
                output[n+2] = __builtin_arm_ssat(__builtin_arm_qadd(first[2], c) >> 15, 16);
                output[n+3] = __builtin_arm_ssat(__builtin_arm_qadd(first[3], d) >> 15, 16);
            }
        }
    }
    memcpy(history, history + USB_SAMPLES, (TAPS - 1) * sizeof(*history));
}
