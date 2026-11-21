/* Four-path search for standard 64 kbit/s G.722 high-band symbols. The predictor
 * follows libg722's block4() equations, writing each survivor directly into its
 * next state. This avoids copying intermediate decoder work areas on Cortex-M4. */
#pragma once

enum { SEARCH_WIDTH = 4, SEARCH_SAMPLES = 8 };
static const int high_inverse[4] = {-7408, -1616, 7408, 1616};

static inline int clamp_high(int value)
{
#ifdef __ARM_FEATURE_DSP
    return __builtin_arm_ssat(value, 15);
#else
    return value < -16384 ? -16384 : value > 16383 ? 16383 : value;
#endif
}

static inline uint32_t packed_samples(const int16_t *samples)
{
    uint32_t pair;
    __builtin_memcpy(&pair, samples, sizeof(pair));
    return pair;
}

static inline uint32_t pack_halves(int low, int high)
{
    return (uint16_t)low | ((uint32_t)high << 16);
}

static inline void predict_delta(const struct g722_high_state *old,
                                 struct g722_high_state *out, int diff)
{
    int r = saturate(old->s + diff);
    int p = saturate(old->sz + diff);
    int pole = saturate(old->a1 * 4);
    int term = ((p ^ old->p1) >= 0) ? -pole : pole;
    if (term > 32767) term = 32767;
    int a2 = (term >> 7) + (((p ^ old->p2) >= 0) ? 128 : -128)
             + ((old->a2 * 32512) >> 15);
    if (a2 > 12288) a2 = 12288;
    if (a2 < -12288) a2 = -12288;
    int a1 = ((old->a1 * 32640) >> 15) + (((p ^ old->p1) >= 0) ? 192 : -192);
    int limit = 15360 - a2;
    if (a1 > limit) a1 = limit;
    if (a1 < -limit) a1 = -limit;
    int sp = saturate(((a1 * saturate(2 * r)) >> 15)
                      + ((a2 * saturate(2 * old->r)) >> 15));
    int sz = 0;
#ifdef __ARM_FEATURE_DSP
    uint32_t sign = diff < 0 ? UINT32_MAX : 0;
    for (int i = 4; i >= 0; i -= 2) {
        uint32_t old_b = packed_samples(old->b + i);
        uint32_t old_d = packed_samples(old->d + i);
        uint32_t leaked = pack_halves(((int16_t)old_b * 32640) >> 15,
                                      ((int16_t)(old_b >> 16) * 32640) >> 15);
        uint32_t opposite = ((old_d ^ sign) & 0x80008000U) >> 7;
        uint32_t adjust = diff ? __builtin_arm_ssub16(0x00800080, opposite) : 0;
        uint32_t b = __builtin_arm_sadd16(leaked, adjust);
        uint32_t d = i ? packed_samples(old->d + i - 1) : pack_halves(diff, old->d[0]);
        __builtin_memcpy(out->d + i, &d, sizeof(d));
        __builtin_memcpy(out->b + i, &b, sizeof(b));
        uint32_t doubled = __builtin_arm_qadd16(d, d);
        sz += (((int16_t)b * (int16_t)doubled) >> 15) + (((int16_t)(b >> 16) * (int16_t)(doubled >> 16)) >> 15);
    }
#else
    int adjust = diff == 0 ? 0 : 128;
    for (int i = 5; i >= 0; --i) {
        int b = ((old->b[i] * 32640) >> 15) + (((diff ^ old->d[i]) >= 0) ? adjust : -adjust);
        int d = i == 0 ? diff : old->d[i - 1];
        out->d[i] = d;
        out->b[i] = b;
        sz += (b * saturate(2 * d)) >> 15;
    }
#endif
    out->sz = saturate(sz);
    out->s = saturate(sp + out->sz);
    out->r = r;
    out->p2 = old->p1;
    out->p1 = p;
    out->a1 = a1;
    out->a2 = a2;
}

/* The G.722 scale depends only on the quantized log step. */
static int16_t high_differences[353][4];
static uint8_t low_threshold[2920];

static void prepare_quantizers(const int *scales, const int *thresholds)
{
    if (low_threshold[0]) return;
    for (unsigned index = 0; index < 353; ++index) {
        int scale = scales[index & 31];
        int shift = 10 - (int)(index >> 5);
        int det = (shift < 0 ? scale << -shift : scale >> shift) * 4;
        for (unsigned s = 0; s < 4; ++s)
            high_differences[index][s] = (det * high_inverse[s]) >> 15;
    }
    unsigned index = 1;
    for (unsigned key = 0; key < 2920; ++key) {
        while ((unsigned)thresholds[index] < key) ++index;
        low_threshold[key] = index;
    }
}

static inline void high_predict(const struct g722_high_state *old, struct g722_high_state *out,
                                unsigned symbol)
{
    int diff = high_differences[old->nb >> 6][symbol];
    int step = ((old->nb * 127) >> 7) + ((symbol & 1) ? -214 : 798);
    if (step < 0) step = 0;
    if (step > 22528) step = 22528;
    out->nb = step;
    predict_delta(old, out, diff);
}

/* Keep the even and odd history contiguous across wrap for packed QMF MACs. */
static inline void quality_qmf(G722_ENC_CTX *s, const int16_t *input, int *low, int *high)
{
    static const int16_t coeff[12] = {3,-11,12,32,-210,951,3876,-805,362,-156,53,-11};
    static const int16_t reversed[12] = {-11,53,-156,362,-805,3876,951,-210,32,12,-11,3};
    unsigned head = s->qmf_head;
    s->even[head] = s->even[head + 12] = input[0];
    s->odd[head] = s->odd[head + 12] = input[1];
    head = head == 11 ? 0 : head + 1;
    s->qmf_head = head;
    int even = 0, odd = 0;
#ifdef __ARM_FEATURE_DSP
    for (unsigned k = 0; k < 12; k += 2) {
        even = __builtin_arm_smlad(packed_samples(s->even + head + k), packed_samples(coeff + k), even);
        odd = __builtin_arm_smlad(packed_samples(s->odd + head + k), packed_samples(reversed + k), odd);
    }
#else
    for (unsigned k = 0; k < 12; ++k) {
        even += s->even[head + k] * coeff[k];
        odd += s->odd[head + k] * reversed[k];
    }
#endif
    *low = (odd + even) >> 14;
    *high = (odd - even) >> 14;
}

struct high_path {
    struct g722_high_state state;
    uint32_t score;
    unsigned symbols;
};
struct high_choice { uint32_t score; unsigned code; };

#if defined(__ZEPHYR__)
__attribute__((noinline))
#endif
static void encode_high_band(struct g722_high_state *band, const int *samples,
                             uint8_t *payload, int count)
{
    struct high_path banks[2][SEARCH_WIDTH];
    struct high_path *current = banks[0], *next = banks[1];
    current[0].score = 0;
    current[0].symbols = 0;
    int live = 1;
    for (int i = 0; i < count; ++i) {
        int last = i + 1 == count;
        int limit = last ? 1 : SEARCH_WIDTH;
        struct high_choice best[SEARCH_WIDTH] = {
            {UINT32_MAX, 0}, {UINT32_MAX, 0}, {UINT32_MAX, 0}, {UINT32_MAX, 0}
        };
        for (int parent = 0; parent < live; ++parent) {
            const struct g722_high_state *from = i ? &current[parent].state : band;
            const int16_t *differences = high_differences[from->nb >> 6];
            for (unsigned symbol = 0; symbol < 4; ++symbol) {
                int diff = differences[symbol];
                int delta = samples[i] - clamp_high(from->s + diff);
                /* |target| <= 25928 for this QMF. Single errors are < 1.8e9.
                 * The cheapest parent's four children bound all four survivors.
                 * Subtracting the best score each step keeps sums below 3.6e9. */
                uint32_t score = current[parent].score + (uint32_t)(delta * delta);
                unsigned code = (unsigned)parent * 4 + symbol;
                if (last) {
                    if (score < best[0].score) best[0] = (struct high_choice){score, code};
                    continue;
                }
                if (score >= best[3].score) continue;
                int at = 3;
                while (at && score < best[at - 1].score) {
                    best[at] = best[at - 1];
                    --at;
                }
                best[at] = (struct high_choice){score, code};
            }
        }
        for (int p = 0; p < limit; ++p) {
            unsigned parent = best[p].code >> 2, symbol = best[p].code & 3;
            unsigned codes = (current[parent].symbols << 2) | symbol;
            high_predict(i ? &current[parent].state : band, last ? band : &next[p].state,
                         symbol);
            if (last) {
                for (int j = count - 1; j >= 0; --j) {
                    payload[j] |= (codes & 3) << 6;
                    codes >>= 2;
                }
                return;
            }
            next[p].score = best[p].score - best[0].score;
            next[p].symbols = codes;
        }
        struct high_path *old = current;
        current = next;
        next = old;
        live = SEARCH_WIDTH;
    }
}
