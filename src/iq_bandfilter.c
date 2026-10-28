#include "iq_bandfilter.h"
#include <fftw3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
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
        if (!f->forward) {
            f->forward = fftwf_plan_dft_1d(NFFT, f->time, f->freq, FFTW_FORWARD, FFTW_ESTIMATE);
            f->inverse = fftwf_plan_dft_1d(NFFT, f->freq, f->time, FFTW_BACKWARD, FFTW_ESTIMATE);
            if (!f->forward || !f->inverse) return -1;
        }
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
    memset(f->history, 0, HISTORY*sizeof(*f->history));
    if (iq_bandfilter_set(f, bandwidth)) goto fail;
    return f;
fail:
    iq_bandfilter_destroy(f); return NULL;
}
void iq_bandfilter_process(IQBandFilter *f, uint8_t *iq, size_t samples, int u8) {
    size_t stride = u8 ? 2 : 4;
    while (samples) {
        size_t n = samples < BLOCK ? samples : BLOCK;
        memcpy(f->time, f->history, HISTORY*sizeof(*f->time));
        for (size_t i = 0; i < n; ++i) for (size_t axis = 0; axis < 2; ++axis) {
            size_t at = i*stride + axis*(u8 ? 1 : 2);
            int v = u8 ? (int)iq[at]-128 : (int)(iq[at] | (unsigned)iq[at+1]<<8);
            if (!u8 && v >= 32768) v -= 65536;
            f->time[HISTORY+i][axis] = (float)v;
        }
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
            for (size_t i = 0; i < n; ++i) for (size_t axis = 0; axis < 2; ++axis) {
                float v = f->time[HISTORY+i][axis]/NFFT;
                int value = (int)lrintf(fmaxf(u8 ? -128 : -32768, fminf(u8 ? 127 : 32767, v)));
                size_t at = i*stride + axis*(u8 ? 1 : 2);
                if (u8) iq[at] = (uint8_t)(value+128);
                else { iq[at] = (uint16_t)value & 255; iq[at+1] = (uint16_t)value >> 8; }
            }
        }
        iq += n*stride; samples -= n;
    }
}
