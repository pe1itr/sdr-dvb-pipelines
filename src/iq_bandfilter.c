#include "iq_bandfilter.h"
#include <fftw3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#define NFFT 16384
#define HISTORY (IQ_FILTER_TAPS-1)
#define BLOCK (NFFT-HISTORY)
#define PI 3.14159265358979323846
struct IQBandFilter {
    double rate, bandwidth;
    fftwf_complex *time, *freq, *response, *history;
    fftwf_plan forward, inverse;
};
int iq_bandfilter_valid(double rate, double bandwidth) {
    return isfinite(rate) && rate > 0 && isfinite(bandwidth) &&
        (bandwidth == 0 || (bandwidth >= fmax(10, rate/256) && bandwidth <= rate*.95));
}
void iq_bandfilter_destroy(IQBandFilter *f) {
    if (!f) return;
    if (f->forward) fftwf_destroy_plan(f->forward);
    if (f->inverse) fftwf_destroy_plan(f->inverse);
    fftwf_free(f->time); fftwf_free(f->freq); fftwf_free(f->response); fftwf_free(f->history); free(f);
}
int iq_bandfilter_set(IQBandFilter *f, double bandwidth) {
    if (!iq_bandfilter_valid(f->rate, bandwidth)) return -1;
    if (bandwidth == f->bandwidth) return 0;
    if (bandwidth > 0) {
        memset(f->time, 0, NFFT*sizeof(*f->time));
        double sum = 0, cutoff = bandwidth/(2*f->rate);
        /* Compute one half then mirror exactly: H(w)=A(w)*exp(-j*w*4096).
         * Blackman window; width changes never change the number of taps. */
        for (size_t i = 0; i <= IQ_FILTER_DELAY; ++i) {
            double x = (double)i - IQ_FILTER_DELAY;
            double window = .42-.5*cos(2*PI*i/HISTORY)+.08*cos(4*PI*i/HISTORY);
            float h = (float)((x == 0 ? 2*cutoff : sin(2*PI*cutoff*x)/(PI*x))*window);
            f->time[i][0] = h; f->time[HISTORY-i][0] = h;
            sum += h*(i == IQ_FILTER_DELAY ? 1 : 2);
        }
        for (size_t i = 0; i < IQ_FILTER_TAPS; ++i) f->time[i][0] /= (float)sum;
        fftwf_execute(f->forward);
        memcpy(f->response, f->freq, NFFT*sizeof(*f->freq));
    }
    /* Keep raw input history across bandwidth changes and bypass. */
    f->bandwidth = bandwidth;
    return 0;
}
IQBandFilter *iq_bandfilter_create(double rate, double bandwidth) {
    if (!iq_bandfilter_valid(rate, bandwidth)) return NULL;
    IQBandFilter *f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->rate = rate;
    f->time = fftwf_alloc_complex(NFFT); f->freq = fftwf_alloc_complex(NFFT);
    f->response = fftwf_alloc_complex(NFFT); f->history = fftwf_alloc_complex(HISTORY);
    if (!f->time || !f->freq || !f->response || !f->history) goto fail;
    /* Plan once before reception, even when initially bypassed: live enabling
     * must not block the SDR read loop for FFT planning. ESTIMATE can select a
     * much slower transform at this size. FFTW retains measured wisdom in the
     * process, so retuning reuses it. Planning only overwrites scratch arrays. */
    f->forward = fftwf_plan_dft_1d(NFFT, f->time, f->freq, FFTW_FORWARD, FFTW_MEASURE);
    f->inverse = fftwf_plan_dft_1d(NFFT, f->freq, f->time, FFTW_BACKWARD, FFTW_MEASURE);
    if (!f->forward || !f->inverse) goto fail;
    memset(f->history, 0, HISTORY*sizeof(*f->history));
    if (iq_bandfilter_set(f, bandwidth)) goto fail;
    return f;
fail:
    iq_bandfilter_destroy(f); return NULL;
}
/* Convert whole runs rather than branch on the format for every I/Q value.
 * The wire formats remain little-endian s16 and unsigned, offset-binary u8.
 * SSE2 is baseline on x86-64; other targets keep the scalar conversion. */
static void unpack(float *out, const uint8_t *in, size_t count, int u8, int floats) {
    if (floats) { memcpy(out,in,count*sizeof(float)); return; }
    size_t i=0;
#if defined(__SSE2__)
    const __m128i zero=_mm_setzero_si128();
    for (;i+8<=count;i+=8) {
        __m128i v;
        if (u8) {
            v=_mm_loadl_epi64((const __m128i *)(in+i));
            v=_mm_sub_epi16(_mm_unpacklo_epi8(v,zero),_mm_set1_epi16(128));
        } else v=_mm_loadu_si128((const __m128i *)(in+2*i));
        __m128i sign=_mm_srai_epi16(v,15);
        _mm_storeu_ps(out+i,_mm_cvtepi32_ps(_mm_unpacklo_epi16(v,sign)));
        _mm_storeu_ps(out+i+4,_mm_cvtepi32_ps(_mm_unpackhi_epi16(v,sign)));
    }
#endif
    for (;i<count;++i) {
        int v=u8 ? (int)in[i]-128 : (int)(in[2*i] | (unsigned)in[2*i+1]<<8);
        if (!u8 && v>=32768) v-=65536;
        out[i]=(float)v;
    }
}
static void pack(uint8_t *out, const float *in, size_t count, int u8, int floats) {
    if (floats) {
        float *values=(float *)out;
        for (size_t i=0;i<count;++i) values[i]=in[i]/NFFT;
        return;
    }
    size_t i=0;
#if defined(__SSE2__)
    const __m128 scale=_mm_set1_ps(1.0f/NFFT);
    const __m128 low=_mm_set1_ps(u8 ? -128 : -32768), high=_mm_set1_ps(u8 ? 127 : 32767);
    for (;i+8<=count;i+=8) {
        __m128 a=_mm_mul_ps(_mm_loadu_ps(in+i),scale);
        __m128 b=_mm_mul_ps(_mm_loadu_ps(in+i+4),scale);
        /* Operand order also preserves fminf/fmaxf's handling of NaN.
         * cvtps uses the active rounding mode, as lrintf does. */
        a=_mm_max_ps(_mm_min_ps(a,high),low);
        b=_mm_max_ps(_mm_min_ps(b,high),low);
        __m128i v=_mm_packs_epi32(_mm_cvtps_epi32(a),_mm_cvtps_epi32(b));
        if (u8) {
            v=_mm_add_epi16(v,_mm_set1_epi16(128));
            v=_mm_packus_epi16(v,v);
            _mm_storel_epi64((__m128i *)(out+i),v);
        } else _mm_storeu_si128((__m128i *)(out+2*i),v);
    }
#endif
    for (;i<count;++i) {
        float v=in[i]/NFFT;
        int value=(int)lrintf(fmaxf(u8 ? -128 : -32768, fminf(u8 ? 127 : 32767,v)));
        if (u8) out[i]=(uint8_t)(value+128);
        else {out[2*i]=(uint16_t)value & 255;out[2*i+1]=(uint16_t)value >> 8;}
    }
}
static void process(IQBandFilter *f, uint8_t *iq, size_t samples, int u8, int floats) {
    size_t stride = floats ? 2*sizeof(float) : u8 ? 2 : 4;
    if (!f->bandwidth) {
        /* Bypass is bit-exact but must retain raw history for a live enable.
         * Only the final HISTORY samples matter, even for large input blocks. */
        size_t keep=samples<HISTORY ? samples : HISTORY;
        memmove(f->history,f->history+keep,(HISTORY-keep)*sizeof(*f->history));
        if (keep) unpack((float *)(f->history+HISTORY-keep),iq+(samples-keep)*stride,2*keep,u8,floats);
        return;
    }
    while (samples) {
        size_t n = samples < BLOCK ? samples : BLOCK;
        memcpy(f->time, f->history, HISTORY*sizeof(*f->time));
        unpack((float *)(f->time+HISTORY),iq,2*n,u8,floats);
        memcpy(f->history, f->time+n, HISTORY*sizeof(*f->history));
        if (f->bandwidth > 0) {
            memset(f->time+HISTORY+n, 0, (BLOCK-n)*sizeof(*f->time));
            fftwf_execute(f->forward);
            for (size_t i = 0; i < NFFT; ++i) {
                float re = f->freq[i][0], im = f->freq[i][1];
                f->freq[i][0] = re*f->response[i][0]-im*f->response[i][1];
                f->freq[i][1] = re*f->response[i][1]+im*f->response[i][0];
            }
            fftwf_execute(f->inverse);
            pack(iq,(const float *)(f->time+HISTORY),2*n,u8,floats);
        }
        iq += n*stride; samples -= n;
    }
}

void iq_bandfilter_process(IQBandFilter *f, uint8_t *iq, size_t samples, int u8) {
    process(f,iq,samples,u8,0);
}
void iq_bandfilter_process_float(IQBandFilter *f, float *iq, size_t samples) {
    process(f,(uint8_t *)iq,samples,0,1);
}
