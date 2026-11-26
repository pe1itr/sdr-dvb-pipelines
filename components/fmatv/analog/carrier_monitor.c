#include "carrier_monitor.h"
#include "../fmatv.h"
#include "sound.h"
#include <SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct CarrierMonitor {
    double rate;
    Sound *mono;
    StereoSound *stereo;
    float *samples;
    size_t window, interval, position;
    SDL_Thread *thread;
    SDL_sem *work;
    SDL_atomic_t busy, stop, detected;
    int skip;
};

/* Monitor-only branch of atv-receiver-analog's nicam_process: compare
 * energy around 6.552 MHz with leakage from the neighbouring FM carrier. */
static int nicam_carrier(const float *samples,size_t n,double rate,double fm_power) {
    const double angle=2*3.141592653589793*6552000/rate;
    const double cs=cos(angle),sn=sin(angle);
    unsigned stride=(unsigned)floor(rate/3e6);
    double intermediate=rate/stride;
    float alpha=1-exp(-2*3.141592653589793*700000/rate);
    float narrow=1-exp(-2*3.141592653589793*100000/intermediate);
    double smooth=1-exp(-1/(intermediate*.030));
    double c=1,s=0,power=0;
    float re[4]={0},im[4]={0},dr[4]={0},di[4]={0};
    unsigned phase=0;
    for (size_t j=0;j<n;++j) {
        float v=isfinite(samples[j]) ? samples[j] : 0;
        float r=v*c,i=-v*s;
        double next=c*cs-s*sn;
        s=s*cs+c*sn; c=next;
        for (unsigned k=0;k<4;++k) {
            re[k]+=alpha*(r-re[k]); im[k]+=alpha*(i-im[k]);
            r=re[k]; i=im[k];
        }
        if (++phase<stride) continue;
        phase=0;
        for (unsigned k=0;k<4;++k) {
            dr[k]+=narrow*(r-dr[k]); di[k]+=narrow*(i-di[k]);
            r=dr[k]; i=di[k];
        }
        power+=smooth*(r*r+i*i-power);
    }
    return power>fmax(1e-6,.02*fm_power);
}

static int scan(void *opaque) {
    CarrierMonitor *m=opaque;
    for (;;) {
        SDL_SemWait(m->work);
        if (SDL_AtomicGet(&m->stop)) break;
        sound_reset(m->mono);
        sound_monitor(m->mono,m->samples,m->window,0);
        unsigned flags=sound_locked(m->mono) ? FMATV_MONO : 0;
        if (nicam_carrier(m->samples,m->window,m->rate,sound_power(m->mono))) flags|=FMATV_NICAM;
        if (m->stereo) {
            stereo_sound_reset(m->stereo,0,0);
            stereo_sound_monitor(m->stereo,m->samples,m->window,0);
            flags|=stereo_sound_detected(m->stereo)*FMATV_AUDIO_702;
        }
        SDL_AtomicSet(&m->detected,flags);
        SDL_AtomicSet(&m->busy,0);
    }
    return 0;
}

CarrierMonitor *carrier_monitor_create(double rate,int stereo) {
    CarrierMonitor *m=calloc(1,sizeof(*m));
    if (!m) return NULL;
    m->rate=rate; m->window=(size_t)(rate*.160); m->interval=(size_t)(rate*2);
    m->mono=sound_create(rate,NULL,NULL);
    if (stereo) m->stereo=stereo_sound_create(rate,NULL,NULL);
    m->samples=malloc(m->window*sizeof(float));
    m->work=SDL_CreateSemaphore(0);
    if (m->samples && m->work && m->mono && (!stereo || m->stereo))
        m->thread=SDL_CreateThread(scan,"PAL audio scan",m);
    if (!m->thread) { carrier_monitor_destroy(m); return NULL; }
    return m;
}

/* On the pre-de-emphasis video thread, copy a continuous 160 ms window
 * every two seconds. A slow monitor must never block video acquisition. */
void carrier_monitor_feed(void *opaque,const float *samples,size_t n) {
    CarrierMonitor *m=opaque;
    while (n) {
        if (!m->position) {
            m->skip=SDL_AtomicGet(&m->busy);
            if (m->skip) SDL_AtomicSet(&m->detected,0);
        }
        int collect=!m->skip && m->position<m->window;
        size_t end=collect ? m->window : m->interval;
        size_t chunk=end-m->position;
        if (chunk>n) chunk=n;
        if (collect) memcpy(m->samples+m->position,samples,chunk*sizeof(float));
        m->position+=chunk; samples+=chunk; n-=chunk;
        if (collect && m->position==m->window) {
            SDL_AtomicSet(&m->busy,1); SDL_SemPost(m->work);
        }
        if (m->position==m->interval) m->position=0;
    }
}
unsigned carrier_monitor_detected(CarrierMonitor *m) {
    return m ? (unsigned)SDL_AtomicGet(&m->detected) : 0;
}
void carrier_monitor_destroy(CarrierMonitor *m) {
    if (!m) return;
    if (m->thread) {
        SDL_AtomicSet(&m->stop,1); SDL_SemPost(m->work); SDL_WaitThread(m->thread,NULL);
    }
    if (m->work) SDL_DestroySemaphore(m->work);
    sound_destroy(m->mono); stereo_sound_destroy(m->stereo);
    free(m->samples); free(m);
}
