#define _POSIX_C_SOURCE 200809L
#include <stddef.h>
#include "iq_monitor.h"
#include "usb_tap.h"
#include <fftw3.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
/* Optional, best-effort local telemetry. Never waits for the GUI. Eight
 * contiguous Hann FFTs per update, at most ten updates per input second. */
static void spectrum_feed(const unsigned char *iq, size_t samples, int u8, double rate, int snapshot) {
    static int initialized, fd = -1, used, averages;
    static size_t skip;
    static struct sockaddr_in address;
    static fftwf_complex *in, *out;
    static fftwf_plan plan;
    static float window[IQ_MONITOR_BINS];
    static double sum[IQ_MONITOR_BINS], window_power;
    static IQMonitorFrame frame;
    if (!initialized) {
        initialized = 1;
        const char *port = getenv("LINRAD_IQ_MONITOR_PORT");
        if (!port) return;
        char *end; long p = strtol(port, &end, 10);
        if (*end || p < 1 || p > 65535) return;
        fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return;
        address = (struct sockaddr_in){.sin_family=AF_INET, .sin_port=htons(p), .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
        in = fftwf_alloc_complex(IQ_MONITOR_BINS); out = fftwf_alloc_complex(IQ_MONITOR_BINS);
        if (in && out) plan = fftwf_plan_dft_1d(IQ_MONITOR_BINS, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
        if (!plan) { close(fd); fd = -1; return; }
        for (int i=0; i<IQ_MONITOR_BINS; ++i) {
            window[i] = .5f - .5f*cosf(6.28318530718f*i/IQ_MONITOR_BINS);
            window_power += window[i]*window[i];
        }
    }
    if (fd < 0 || rate <= 0) return;
    size_t stride = u8 ? 2 : 4;
    while (samples) {
        if (skip) {
            size_t n = samples < skip ? samples : skip;
            iq += n*stride; samples -= n; skip -= n; continue;
        }
        for (int axis=0; axis<2; ++axis) {
            int v = u8 ? (int)iq[axis]-128 : (int)(iq[axis*2] | (unsigned)iq[axis*2+1]<<8);
            if (!u8 && v >= 32768) v -= 65536;
            in[used][axis] = v/(u8 ? 128.f : 32768.f)*window[used];
        }
        iq += stride; --samples;
        if (++used != IQ_MONITOR_BINS) continue;
        used = 0; fftwf_execute(plan);
        for (int i=0; i<IQ_MONITOR_BINS; ++i) sum[i] += (double)out[i][0]*out[i][0]+(double)out[i][1]*out[i][1];
        if (++averages != 8) continue;
        frame.magic = IQ_MONITOR_MAGIC; frame.rate = rate; ++frame.sequence;
        for (int i=0; i<IQ_MONITOR_BINS; ++i) {
            frame.power[(i+IQ_MONITOR_BINS/2)%IQ_MONITOR_BINS] = 10*log10(fmax(sum[i]/(8*rate*window_power), 1e-20));
            sum[i] = 0;
        }
        (void)sendto(fd, &frame, sizeof(frame), MSG_DONTWAIT, (struct sockaddr *)&address, sizeof(address));
        averages = 0;
        skip = !snapshot && rate/10 > 8*IQ_MONITOR_BINS ? (size_t)(rate/10)-8*IQ_MONITOR_BINS : 0;
    }
}

/* Only 8 contiguous FFT windows per update are copied, not the full IQ rate.
 * One pending snapshot plus one in-flight snapshot bounds memory/latency.
 * The producer never waits for the spectrum worker or its UDP output. */
#define SNAPSHOT_SAMPLES (8*IQ_MONITOR_BINS)
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t thread;
    int active, ready, stop, pending, pending_u8, capture_u8;
    double pending_rate, capture_rate;
    size_t used, skip;
    uint64_t dropped;
    unsigned char capture[SNAPSHOT_SAMPLES*4], queued[SNAPSHOT_SAMPLES*4];
} monitor;

static void *spectrum_worker(void *unused) {
    (void)unused;
    unsigned char data[SNAPSHOT_SAMPLES*4];
    pthread_mutex_lock(&monitor.mutex);
    monitor.ready=1;pthread_cond_signal(&monitor.changed);
    for (;;) {
        while (!monitor.pending && !monitor.stop) pthread_cond_wait(&monitor.changed, &monitor.mutex);
        if (!monitor.pending && monitor.stop) break;
        int u8=monitor.pending_u8; double rate=monitor.pending_rate;
        memcpy(data, monitor.queued, SNAPSHOT_SAMPLES*(u8 ? 2 : 4));
        monitor.pending=0;
        pthread_mutex_unlock(&monitor.mutex);
        spectrum_feed(data, SNAPSHOT_SAMPLES, u8, rate, 1);
        pthread_mutex_lock(&monitor.mutex);
    }
    pthread_mutex_unlock(&monitor.mutex);
    return NULL;
}

int iq_monitor_start_async(void) {
    if (monitor.active) return 1;
    const char *port=getenv("LINRAD_IQ_MONITOR_PORT"), *sync=getenv("LINRAD_IQ_MONITOR_SYNC");
    if (!port || !*port || (sync && !strcmp(sync,"1"))) return 0;
    /* Plan allocation happens before live streaming, never during recv. */
    spectrum_feed(NULL,0,0,1,1);
    memset(&monitor,0,sizeof(monitor));
    if (pthread_mutex_init(&monitor.mutex,NULL)) return -1;
    if (pthread_cond_init(&monitor.changed,NULL)) {pthread_mutex_destroy(&monitor.mutex);return -1;}
    if (pthread_create(&monitor.thread,NULL,spectrum_worker,NULL)) {
        pthread_cond_destroy(&monitor.changed);pthread_mutex_destroy(&monitor.mutex);return -1;
    }
    pthread_mutex_lock(&monitor.mutex);
    while (!monitor.ready) pthread_cond_wait(&monitor.changed,&monitor.mutex);
    pthread_mutex_unlock(&monitor.mutex);
    monitor.active=1; return 1;
}

void iq_monitor_stop_async(void) {
    if (!monitor.active) return;
    pthread_mutex_lock(&monitor.mutex);monitor.stop=1;
    pthread_cond_signal(&monitor.changed);pthread_mutex_unlock(&monitor.mutex);
    pthread_join(monitor.thread,NULL);
    pthread_cond_destroy(&monitor.changed);pthread_mutex_destroy(&monitor.mutex);
    monitor.active=0;
}

uint64_t iq_monitor_dropped(void) { return monitor.dropped; }

void iq_monitor_feed(const unsigned char *iq, size_t samples, int u8, double rate) {
    usb_tap_feed(iq,samples,u8,rate);
    if (!monitor.active) {spectrum_feed(iq,samples,u8,rate,0);return;}
    if (!(rate>0) || !isfinite(rate)) return;
    if (rate!=monitor.capture_rate || u8!=monitor.capture_u8) {
        monitor.used=monitor.skip=0;monitor.capture_rate=rate;monitor.capture_u8=u8;
    }
    size_t stride=u8 ? 2 : 4;
    while (samples) {
        if (monitor.skip) {
            size_t n=samples<monitor.skip ? samples : monitor.skip;
            iq+=n*stride;samples-=n;monitor.skip-=n;continue;
        }
        size_t n=SNAPSHOT_SAMPLES-monitor.used;
        if (n>samples) n=samples;
        memcpy(monitor.capture+monitor.used*stride,iq,n*stride);
        monitor.used+=n;iq+=n*stride;samples-=n;
        if (monitor.used!=SNAPSHOT_SAMPLES) continue;
        if (pthread_mutex_trylock(&monitor.mutex)) ++monitor.dropped;
        else {
            if (monitor.pending) ++monitor.dropped;
            else {
                memcpy(monitor.queued,monitor.capture,SNAPSHOT_SAMPLES*stride);
                monitor.pending_u8=u8;monitor.pending_rate=rate;monitor.pending=1;
                pthread_cond_signal(&monitor.changed);
            }
            pthread_mutex_unlock(&monitor.mutex);
        }
        monitor.used=0;
        monitor.skip=rate/10>SNAPSHOT_SAMPLES ? (size_t)(rate/10)-SNAPSHOT_SAMPLES : 0;
    }
}
