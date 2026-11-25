#include "usb_demod.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <samplerate.h>
#define PI 3.14159265358979323846
#define TAPS 513
#define AUDIO_TAPS 257
struct UsbDemod {
    MonitorMode mode;
    unsigned decimation, phase, position, normalize;
    double ci, cq, si, sq, ai, aq, asi, asq, scale;
    uint64_t integrator[2][4], comb[2][4];
    float taps[TAPS], hi[TAPS], hq[TAPS], input[256];
    unsigned used;
    double ratio, level, gain;
    SRC_STATE *resampler;
    double previous_i, previous_q, discriminator_gain, deemphasis, deemphasis_alpha;
    double highpass, highpass_previous, highpass_alpha;
    float audio_taps[AUDIO_TAPS], audio_history[AUDIO_TAPS];
    unsigned audio_position;
    int squelch_enabled, squelch_open;
    unsigned window_samples, window_used, bad_windows;
    double power_sum, power_squared_sum;
};
UsbDemod *usb_demod_new(double rate, double offset) {
    return usb_demod_new_mode(rate,offset,MONITOR_USB);
}
UsbDemod *usb_demod_new_mode(double rate, double offset, MonitorMode mode) {
    if ((mode!=MONITOR_USB && mode!=MONITOR_FM && mode!=MONITOR_FM_DATA) || !isfinite(rate) ||
        rate<monitor_min_rate(mode) || rate>100000000 || !isfinite(offset) ||
        offset+monitor_low(mode)<-rate/2 || offset+monitor_high(mode)>rate/2) return NULL;
    UsbDemod *d=calloc(1,sizeof(*d));
    if (!d) return NULL;
    d->mode=mode;
    d->decimation=(unsigned)fmax(1,floor(rate/(mode==MONITOR_USB ? 24000 : 48000)));
    double intermediate=rate/d->decimation;
    double angle=-2*PI*(offset+(mode==MONITOR_USB ? 1500 : 0))/rate;
    d->ci=d->ai=1; d->si=cos(angle); d->sq=sin(angle);
    d->asi=cos(2*PI*1500/intermediate); d->asq=sin(2*PI*1500/intermediate);
    d->scale=1/(8192.0*pow(d->decimation,4));
    d->ratio=48000/intermediate; d->gain=1;
    /* DATA has fixed scaling with headroom: +/-5 kHz deviation gives +/-0.25
     * after output gain. Preserve DC and the discriminator waveform. */
    d->discriminator_gain=intermediate/(2*PI*(mode==MONITOR_FM_DATA ? 10000 : 2500));
    d->deemphasis_alpha=1-exp(-1/(intermediate*75e-6));
    d->highpass_alpha=exp(-2*PI*300/intermediate);
    double cutoff=mode==MONITOR_USB ? 1500 : monitor_high(mode);
    double sum=0;
    for (int i=0;i<TAPS;++i) {
        double t=i-(TAPS-1)/2;
        double v=t==0 ? 2*cutoff/intermediate : sin(2*PI*cutoff/intermediate*t)/(PI*t);
        v*=.42-.5*cos(2*PI*i/(TAPS-1))+.08*cos(4*PI*i/(TAPS-1));
        d->taps[i]=v; sum+=v;
    }
    for (int i=0;i<TAPS;++i) d->taps[i]/=sum;
    if (mode==MONITOR_FM) {
        d->window_samples=(unsigned)lround(intermediate*.020);
        sum=0;
        for (int i=0;i<AUDIO_TAPS;++i) {
            double t=i-(AUDIO_TAPS-1)/2;
            double v=t==0 ? 6000/intermediate : sin(2*PI*3000/intermediate*t)/(PI*t);
            v*=.42-.5*cos(2*PI*i/(AUDIO_TAPS-1))+.08*cos(4*PI*i/(AUDIO_TAPS-1));
            d->audio_taps[i]=v; sum+=v;
        }
        for (int i=0;i<AUDIO_TAPS;++i) d->audio_taps[i]/=sum;
    }
    int error;
    d->resampler=src_new(SRC_SINC_FASTEST,1,&error);
    if (!d->resampler) { free(d); return NULL; }
    return d;
}
void usb_demod_set_fm_squelch(UsbDemod *d, int enabled) {
    enabled=enabled && d->mode==MONITOR_FM;
    if (enabled==d->squelch_enabled) return;
    d->squelch_enabled=enabled;
    d->squelch_open=0;
    d->window_used=d->bad_windows=0;
    d->power_sum=d->power_squared_sum=0;
}

/* A clean FM carrier has a nearly constant envelope. For Gaussian channel
 * noise, mean(power)^2 / mean(power^2) approaches 0.5, independent of gain.
 * Evaluate every 20 ms and hold through two noisy windows before closing. */
static int fm_carrier_present(UsbDemod *d, double power) {
    d->power_sum+=power;
    d->power_squared_sum+=power*power;
    if (++d->window_used==d->window_samples) {
        double energy=d->window_samples*d->power_squared_sum;
        double coherence=d->power_sum*d->power_sum;
        if (energy>0 && coherence>energy*(d->squelch_open ? .62 : .72)) {
            d->squelch_open=1;
            d->bad_windows=0;
        } else if (d->squelch_open && ++d->bad_windows>=3) {
            d->squelch_open=0;
        }
        d->window_used=0;
        d->power_sum=d->power_squared_sum=0;
    }
    return d->squelch_open;
}
void usb_demod_free(UsbDemod *d) {
    if (d) { src_delete(d->resampler); free(d); }
}
static void output(UsbDemod *d, UsbAudioCallback callback, void *user) {
    float audio[2048];
    SRC_DATA data={.data_in=d->input,.input_frames=d->used,.data_out=audio,
        .output_frames=2048,.src_ratio=d->ratio};
    /* At most 256 input samples and a ratio <= 6, so all input fits. */
    if (!src_process(d->resampler,&data)) {
        for (long i=0;i<data.output_frames_gen;++i) {
            double value=audio[i];
            if (d->mode==MONITOR_USB) {
                d->level+=.0002*(value*value-d->level);
                double target=fmin(1000,.15/sqrt(fmax(d->level,1e-12)));
                d->gain+=(target<d->gain ? .01 : .00002)*(target-d->gain);
            } else d->gain=.5;
            audio[i]=fmax(-.95,fmin(.95,value*d->gain));
        }
        if (data.output_frames_gen) callback(audio,(size_t)data.output_frames_gen,user);
    }
    d->used=0;
}
void usb_demod_feed(UsbDemod *d, const unsigned char *iq, size_t samples, int u8,
                    UsbAudioCallback callback, void *user) {
    for (size_t n=0;n<samples;++n) {
        int re,im;
        if (u8) { re=((int)iq[0]-128)*256; im=((int)iq[1]-128)*256; iq+=2; }
        else {
            re=iq[0]|(unsigned)iq[1]<<8; im=iq[2]|(unsigned)iq[3]<<8; iq+=4;
            if (re>=32768) re-=65536;
            if (im>=32768) im-=65536;
        }
        /* Center the wanted channel at DC. Unsigned CIC integrators
         * intentionally wrap; comb cancellation restores the signed result. */
        int64_t vi=(int64_t)((re*d->ci-im*d->cq)*.25);
        int64_t vq=(int64_t)((re*d->cq+im*d->ci)*.25);
        double ci=d->ci*d->si-d->cq*d->sq;
        d->cq=d->ci*d->sq+d->cq*d->si; d->ci=ci;
        d->integrator[0][0]+=(uint64_t)vi; d->integrator[1][0]+=(uint64_t)vq;
        for (int j=1;j<4;++j) for (int axis=0;axis<2;++axis)
            d->integrator[axis][j]+=d->integrator[axis][j-1];
        if (++d->normalize==4096) {
            double norm=hypot(d->ci,d->cq); d->ci/=norm; d->cq/=norm; d->normalize=0;
        }
        if (++d->phase<d->decimation) continue;
        d->phase=0;
        for (int axis=0;axis<2;++axis) {
            uint64_t value=d->integrator[axis][3];
            for (int j=0;j<4;++j) {
                uint64_t next=value-d->comb[axis][j]; d->comb[axis][j]=value; value=next;
            }
            int64_t signed_value; memcpy(&signed_value,&value,sizeof(value));
            (axis ? d->hq : d->hi)[d->position]=(float)(signed_value*d->scale);
        }
        double fi=0,fq=0;
        unsigned p=d->position;
        for (int j=0;j<TAPS;++j) {
            fi+=d->hi[p]*d->taps[j]; fq+=d->hq[p]*d->taps[j];
            p=p ? p-1 : TAPS-1;
        }
        d->position=(d->position+1)%TAPS;
        if (d->mode!=MONITOR_USB) {
            /* Quadrature phase difference rejects carrier amplitude changes.
             * Communication FM: +/-2.5 kHz deviation, 300..3000 Hz speech,
             * 75 us de-emphasis; no RF-amplitude AGC in the audio path. */
            double re=fi*d->previous_i+fq*d->previous_q;
            double im=fq*d->previous_i-fi*d->previous_q;
            double value=(re*re+im*im)>1e-28 ? atan2(im,re)*d->discriminator_gain : 0;
            d->previous_i=fi; d->previous_q=fq;
            if (d->mode==MONITOR_FM_DATA) {
                /* No speech high/low-pass, de-emphasis, AGC or squelch.
                 * The common resampler still supplies anti-alias filtering. */
                d->input[d->used++]=value;
                if (d->used==256) output(d,callback,user);
                continue;
            }
            d->deemphasis+=d->deemphasis_alpha*(value-d->deemphasis);
            d->highpass=d->highpass_alpha*(d->highpass+d->deemphasis-d->highpass_previous);
            d->highpass_previous=d->deemphasis;
            d->audio_history[d->audio_position]=d->highpass;
            double filtered=0; unsigned pos=d->audio_position;
            for (int j=0;j<AUDIO_TAPS;++j) {
                filtered+=d->audio_history[pos]*d->audio_taps[j]; pos=pos ? pos-1 : AUDIO_TAPS-1;
            }
            d->audio_position=(d->audio_position+1)%AUDIO_TAPS;
            if (d->squelch_enabled && !fm_carrier_present(d,fi*fi+fq*fq)) filtered=0;
            d->input[d->used++]=filtered;
        } else {
            d->input[d->used++]=(float)(2*(fi*d->ai-fq*d->aq));
            double ai=d->ai*d->asi-d->aq*d->asq;
            d->aq=d->ai*d->asq+d->aq*d->asi; d->ai=ai;
        }
        if (d->used==256) output(d,callback,user);
    }
}
