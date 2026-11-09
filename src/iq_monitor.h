#ifndef IQ_MONITOR_H
#define IQ_MONITOR_H
#include <stdint.h>
#include <stddef.h>
#define IQ_MONITOR_BINS 2048
#define IQ_MONITOR_MAGIC 0x49515331u
typedef struct {
    uint32_t magic;
    uint32_t sequence;
    double rate;
    float power[IQ_MONITOR_BINS]; /* FFT-shifted dBFS/Hz, complex full scale = 1 */
} IQMonitorFrame;
void iq_monitor_feed(const unsigned char *iq, size_t samples, int u8, double rate);
/* Optional spectrum worker for live SDR input. Audio tap stays synchronous.
 * Start before feeding; stop drains the last complete spectrum snapshot.
 * Returns 1 when active, 0 when disabled, -1 on thread creation failure.
 * LINRAD_IQ_MONITOR_SYNC=1 retains the synchronous path for timing comparisons. */
int iq_monitor_start_async(void);
void iq_monitor_stop_async(void);
uint64_t iq_monitor_dropped(void); /* spectrum snapshots, never decoder IQ */
#endif
