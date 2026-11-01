#ifndef IQ_BANDFILTER_H
#define IQ_BANDFILTER_H
#include <stddef.h>
#include <stdint.h>
#define IQ_FILTER_TAPS 8193
#define IQ_FILTER_DELAY 4096
/* Symmetric real FIR on complex IQ: fixed delay, no resampling or IQ rotation.
 * bandwidth is total -6 dB width in Hz; zero is bit-exact bypass. */
typedef struct IQBandFilter IQBandFilter;
int iq_bandfilter_valid(double rate, double bandwidth);
IQBandFilter *iq_bandfilter_create(double rate, double bandwidth);
int iq_bandfilter_set(IQBandFilter *filter, double bandwidth);
void iq_bandfilter_process(IQBandFilter *filter, uint8_t *iq, size_t samples, int u8);
/* Interleaved float IQ; no quantization or clipping before the final output. */
void iq_bandfilter_process_float(IQBandFilter *filter, float *iq, size_t samples);
void iq_bandfilter_destroy(IQBandFilter *filter);
#endif
