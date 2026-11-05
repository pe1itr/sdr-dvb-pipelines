#ifndef IQ_SHIFT_H
#define IQ_SHIFT_H
#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* Positive receiver offset selects above the hardware center: mix DOWN.
 * No DC subtraction, notch, resampling, or automatic gain is performed. */
typedef struct {
    double offset, re, im, step_re, step_im;
    unsigned phase_count;
} IQShift;

/* Leave 2.5% of the source rate at each analog/anti-alias filter edge.
 * Width is the occupied channel (or enabled IQ filter) width in Hz.
 * The channel must fit even when the digital mixer is bypassed. */
static inline int iq_shift_fits(double rate, double offset, double width) {
    return isfinite(rate) && rate > 0 && isfinite(offset) && isfinite(width) && width >= 0 &&
        fabs(offset) + width/2 <= rate*.475;
}
static inline int iq_shift_init(IQShift *s, double rate, double offset) {
    if (!iq_shift_fits(rate, offset, 0)) return -1;
    *s = (IQShift){.offset=offset, .re=1};
    if (offset != 0) {
        double angle = -6.28318530717958647693*offset/rate;
        s->step_re = cos(angle); s->step_im = sin(angle);
    }
    return 0;
}

/* IQ components use native s16 units in float, preserving headroom until
 * after the channel filter. Phase is continuous across arbitrary read sizes. */
static inline void iq_shift_process(IQShift *s, float *iq, size_t samples) {
    if (s->offset == 0) return;
    double re=s->re, im=s->im;
    for (size_t n=0; n<samples; ++n) {
        double i=iq[2*n], q=iq[2*n+1];
        iq[2*n] = (float)(i*re-q*im);
        iq[2*n+1] = (float)(i*im+q*re);
        double next_re=re*s->step_re-im*s->step_im;
        im=im*s->step_re+re*s->step_im; re=next_re;
        if (++s->phase_count == 4096) {
            double norm=hypot(re,im); re/=norm; im/=norm; s->phase_count=0;
        }
    }
    s->re=re; s->im=im;
}
/* Round symmetrically, especially for u8: truncation would introduce a new
 * -0.5 LSB DC bias after shifting the original hardware spur away. */
static inline void iq_shift_pack(const float *iq, unsigned char *out, size_t samples, int u8) {
    for (size_t n=0; n<samples*2; ++n) {
        double v=u8 ? iq[n]/256.0 : iq[n];
        long value=lrint(fmax(u8 ? -128 : -32768, fmin(u8 ? 127 : 32767,v)));
        if (u8) out[n]=(unsigned char)(value+128);
        else { out[2*n]=(uint16_t)value & 255; out[2*n+1]=(uint16_t)value >> 8; }
    }
}
#endif
