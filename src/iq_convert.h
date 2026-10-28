#ifndef IQ_CONVERT_H
#define IQ_CONVERT_H
#include <stddef.h>
#include <stdint.h>
#include <math.h>

/* Host samples are interleaved I,Q. Count is complex samples, not components.
 * No swap, conjugation or gain is applied. s16 output is passed through unchanged. */
static inline void iq_cs8_to_u8(unsigned char *iq, size_t samples) {
    for (size_t i = 0; i < samples*2; ++i) iq[i] ^= 0x80;
}
static inline void iq_cs16_to_u8(const int16_t *iq, unsigned char *out, size_t samples) {
    for (size_t i = 0; i < samples*2; ++i)
        out[i] = (unsigned char)(((int)iq[i]+32768)/256);
}
/* Match the SDR-IQ Python reference: clip CF32, scale by 32767, round to
 * nearest (ties to even in the default rounding mode). Preserve I,Q order. */
static inline void iq_cf32_to_s16(const float *iq, int16_t *out, size_t samples) {
    for (size_t i = 0; i < samples*2; ++i) {
        float v = iq[i];
        if (isnan(v)) v = 0;
        if (v > 1) v = 1;
        if (v < -1) v = -1;
        out[i] = (int16_t)lrintf(v * 32767.0f);
    }
}
#endif
