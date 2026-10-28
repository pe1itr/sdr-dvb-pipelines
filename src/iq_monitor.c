#define _POSIX_C_SOURCE 200809L
#include <stddef.h>
#include "iq_monitor.h"
#include <fftw3.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
/* Optional, best-effort local telemetry. Never waits for the GUI. Eight
 * contiguous Hann FFTs per update, at most ten updates per input second. */
void iq_monitor_feed(const unsigned char *iq, size_t samples, int u8, double rate) {
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
        skip = rate/10 > 8*IQ_MONITOR_BINS ? (size_t)(rate/10)-8*IQ_MONITOR_BINS : 0;
    }
}
