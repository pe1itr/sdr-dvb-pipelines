#ifndef SDR_RX_STATUS_H
#define SDR_RX_STATUS_H
#include <inttypes.h>
#include <glib.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* One report per second, carried on the existing source stderr pipe. Times
 * are wall time, NOT CPU time. A service gap includes both application work
 * outside recv and time for which the OS did not schedule this thread. */
typedef struct {
    uint64_t calls, samples, overflows, timeouts, output_dropped;
    uint64_t gaps_over_5ms, spectrum_dropped;
    double rate, interval_ms, recv_mean_ms, recv_max_ms, gap_max_ms;
    double call_interval_max_ms, overflow_gap_max_ms, monitor_max_ms, output_max_ms;
    int spectrum_async;
} SdrRxStatus;

/* The GUI may use a decimal-comma locale; the SDR process emits C locale. */
static inline int sdr_rx_token(const char **p, const char *key, char value[96]) {
    size_t n=strlen(key);
    if (strncmp(*p,key,n) || (*p)[n]!='=') return 0;
    *p+=n+1; n=strcspn(*p," ");
    if (!n || n>=96 || **p<'0' || **p>'9') return 0;
    memcpy(value,*p,n);value[n]=0;*p+=n;
    if (**p==' ') ++*p;
    return 1;
}
static inline int sdr_rx_status_parse(const char *line, SdrRxStatus *out) {
    const char *p=strstr(line,"[SDR] RX timing: ");
    if (!p) return 0; /* UHD may prefix a complete line with O's. */
    p+=strlen("[SDR] RX timing: ");
    SdrRxStatus s={0};char value[96], *end;
    const char *counts[]={"calls","samples","overflows","timeouts","output_dropped","gaps_over_5ms","spectrum_dropped"};
    uint64_t *dest[]={&s.calls,&s.samples,&s.overflows,&s.timeouts,&s.output_dropped,&s.gaps_over_5ms,&s.spectrum_dropped};
    for (unsigned i=0;i<G_N_ELEMENTS(counts);++i) {
        if (!sdr_rx_token(&p,counts[i],value)) return 0;
        errno=0;*dest[i]=g_ascii_strtoull(value,&end,10);
        if (errno || *end) return 0;
    }
    const char *times[]={"rate","interval_ms","recv_mean_ms","recv_max_ms","gap_max_ms","call_interval_max_ms","overflow_gap_max_ms","monitor_max_ms","output_max_ms"};
    double *values[]={&s.rate,&s.interval_ms,&s.recv_mean_ms,&s.recv_max_ms,&s.gap_max_ms,&s.call_interval_max_ms,&s.overflow_gap_max_ms,&s.monitor_max_ms,&s.output_max_ms};
    for (unsigned i=0;i<G_N_ELEMENTS(times);++i) {
        if (!sdr_rx_token(&p,times[i],value)) return 0;
        errno=0;*values[i]=g_ascii_strtod(value,&end);
        if (errno || *end || !isfinite(*values[i]) || *values[i]<0) return 0;
    }
    if (!sdr_rx_token(&p,"spectrum_async",value) || *p ||
        (strcmp(value,"0") && strcmp(value,"1")) || s.rate<=0 || s.interval_ms<=0) return 0;
    s.spectrum_async=value[0]=='1';*out=s;return 1;
}

typedef struct {
    SdrRxStatus status;
    double last_start, last_end, current_gap, recv_sum_ms;
    uint64_t window_calls;
} SdrRxTiming;

static inline void sdr_rx_begin(SdrRxTiming *t, double now) {
    double gap = t->last_end ? fmax(0, (now-t->last_end)*1000) : 0;
    double interval = t->last_start ? fmax(0, (now-t->last_start)*1000) : 0;
    t->current_gap = gap;
    t->status.gap_max_ms = fmax(t->status.gap_max_ms, gap);
    t->status.call_interval_max_ms = fmax(t->status.call_interval_max_ms, interval);
    if (gap>5) ++t->status.gaps_over_5ms;
    t->last_start = now;
}
static inline void sdr_rx_end(SdrRxTiming *t, double now, size_t samples,
                              int overflow, int timeout) {
    double elapsed = fmax(0, (now-t->last_start)*1000);
    t->last_end = now; ++t->window_calls; ++t->status.calls;
    t->status.samples += samples;
    t->status.overflows += !!overflow; t->status.timeouts += !!timeout;
    t->recv_sum_ms += elapsed;
    t->status.recv_max_ms = fmax(t->status.recv_max_ms, elapsed);
    if (overflow) t->status.overflow_gap_max_ms = fmax(t->status.overflow_gap_max_ms, t->current_gap);
}
static inline void sdr_rx_report(FILE *f, SdrRxTiming *t, double rate, double elapsed,
                                  uint64_t dropped, uint64_t spectrum_dropped, int async) {
    SdrRxStatus *s = &t->status;
    s->rate=rate; s->interval_ms=elapsed*1000; s->output_dropped=dropped;
    s->spectrum_dropped=spectrum_dropped; s->spectrum_async=async;
    s->recv_mean_ms=t->window_calls ? t->recv_sum_ms/t->window_calls : 0;
    fprintf(f, "[SDR] RX timing: calls=%" PRIu64 " samples=%" PRIu64
        " overflows=%" PRIu64 " timeouts=%" PRIu64 " output_dropped=%" PRIu64
        " gaps_over_5ms=%" PRIu64 " spectrum_dropped=%" PRIu64
        " rate=%.3f interval_ms=%.3f recv_mean_ms=%.3f recv_max_ms=%.3f gap_max_ms=%.3f"
        " call_interval_max_ms=%.3f overflow_gap_max_ms=%.3f monitor_max_ms=%.3f output_max_ms=%.3f spectrum_async=%d\n",
        s->calls,s->samples,s->overflows,s->timeouts,s->output_dropped,
        s->gaps_over_5ms,s->spectrum_dropped,s->rate,s->interval_ms,s->recv_mean_ms,
        s->recv_max_ms,s->gap_max_ms,s->call_interval_max_ms,s->overflow_gap_max_ms,
        s->monitor_max_ms,s->output_max_ms,s->spectrum_async);
    t->window_calls=0; t->recv_sum_ms=0;
    s->recv_max_ms=s->gap_max_ms=s->call_interval_max_ms=s->overflow_gap_max_ms=0;
    s->monitor_max_ms=s->output_max_ms=0;
}
#endif
