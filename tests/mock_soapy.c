/* LD_PRELOAD fixture: exercise the real SDR helper without opening hardware. */
#define _POSIX_C_SOURCE 200809L
#include <SoapySDR/Device.h>
#include <SoapySDR/Errors.h>
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double rate, gain;
static int handle, reads, native_s16;
SoapySDRDevice *SoapySDRDevice_make(const SoapySDRKwargs *args) {
    fprintf(stderr,"MOCK open %s\n",SoapySDRKwargs_get(args,"driver"));
    return (SoapySDRDevice *)&handle;
}
int SoapySDRDevice_unmake(SoapySDRDevice *d) { return 0; }
char *SoapySDRDevice_getHardwareKey(const SoapySDRDevice *d) { return strdup(getenv("MOCK_MODEL")); }
SoapySDRRange *SoapySDRDevice_getFrequencyRange(const SoapySDRDevice *d, int dir, size_t ch, size_t *n) {
    *n=1;
    SoapySDRRange *r=calloc(1,sizeof(*r)); r->minimum=1000; r->maximum=2000000000;
    return r;
}
double *SoapySDRDevice_listSampleRates(const SoapySDRDevice *d, int dir, size_t ch, size_t *n) {
    int r2=!strcmp(getenv("MOCK_MODEL"),"Airspy");
    *n=r2 ? 2 : 1; double *r=malloc(*n*sizeof(*r));
    r[0]=r2 ? 10000000 : 768000;
    if (r2) r[1]=getenv("MOCK_NO_2M5") ? 3000000 : 2500000;
    return r;
}
int SoapySDRDevice_setSampleRate(SoapySDRDevice *d, int dir, size_t ch, double value) { rate=value; return 0; }
double SoapySDRDevice_getSampleRate(const SoapySDRDevice *d, int dir, size_t ch) { return getenv("MOCK_RATE") ? atof(getenv("MOCK_RATE")) : rate; }
int SoapySDRDevice_setBandwidth(SoapySDRDevice *d, int dir, size_t ch, double v) {
    assert(strcmp(getenv("MOCK_MODEL"),"Airspy")); return 0;
}
int SoapySDRDevice_setFrequency(SoapySDRDevice *d, int dir, size_t ch, double v, const SoapySDRKwargs *args) {
    fprintf(stderr,"MOCK frequency %.0f\n",v); return 0;
}
int SoapySDRDevice_setGainMode(SoapySDRDevice *d, int dir, size_t ch, bool automatic) {
    fprintf(stderr,"MOCK agc %d\n",automatic); return 0;
}
int SoapySDRDevice_setGain(SoapySDRDevice *d, int dir, size_t ch, double v) {
    assert(v>=0 && v<=45); fprintf(stderr,"MOCK gain aggregate %.0f\n",v); gain=v; return 0;
}
double SoapySDRDevice_getGain(const SoapySDRDevice *d, int dir, size_t ch) { return gain; }
int SoapySDRDevice_setGainElement(SoapySDRDevice *d, int dir, size_t ch, const char *name, double v) {
    fprintf(stderr,"MOCK gain %s %.0f\n",name,v); gain=v; return 0;
}
double SoapySDRDevice_getGainElement(const SoapySDRDevice *d, int dir, size_t ch, const char *name) { return gain; }
SoapySDRStream *SoapySDRDevice_setupStream(SoapySDRDevice *d, int dir, const char *format,
        const size_t *channels, size_t n, const SoapySDRKwargs *args) {
    native_s16=!strcmp(getenv("MOCK_MODEL"),"Airspy");
    assert(!strcmp(format,native_s16 ? "CS16" : "CF32") && n==1 && *channels==0);
    return (SoapySDRStream *)&handle;
}
int SoapySDRDevice_activateStream(SoapySDRDevice *d, SoapySDRStream *s, int flags, long long time, size_t n) {
    fprintf(stderr,"MOCK streaming\n"); return 0;
}
int SoapySDRDevice_readStream(SoapySDRDevice *d, SoapySDRStream *s, void *const *buffers,
        size_t n, int *flags, long long *time, long timeout) {
    if (reads++) {
        /* Give the output queue time to drain before stopping. */
        struct timespec delay={.tv_nsec=50000000}; nanosleep(&delay,NULL);
        raise(SIGTERM); return SOAPY_SDR_TIMEOUT;
    }
    assert(n>=2);
    if (native_s16) {
        int16_t *iq=buffers[0]; iq[0]=32767; iq[1]=-32768; iq[2]=16384; iq[3]=-16384;
    } else {
        float *iq=buffers[0]; iq[0]=1; iq[1]=-1; iq[2]=.5; iq[3]=-.5;
    }
    return 2;
}
int SoapySDRDevice_deactivateStream(SoapySDRDevice *d, SoapySDRStream *s, int flags, long long time) { return 0; }
int SoapySDRDevice_closeStream(SoapySDRDevice *d, SoapySDRStream *s) { return 0; }
