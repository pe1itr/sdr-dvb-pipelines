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
#endif
