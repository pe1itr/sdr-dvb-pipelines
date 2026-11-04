#ifndef LIME_RX_H
#define LIME_RX_H
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define LIME_RX_MIN_RATE 100000
#define LIME_RX_MAX_RATE 30720000
#define LIME_RX_MIN_FREQUENCY 10000000
#define LIME_RX_MAX_FREQUENCY 3500000000.0
/* LimeSuite's unsigned RX gain scale, including its 12 dB offset. */
#define LIME_RX_MAX_GAIN 73
static inline int lime_rx_valid(double rate, double frequency, double gain) {
    return isfinite(rate) && rate >= LIME_RX_MIN_RATE && rate <= LIME_RX_MAX_RATE && rate == floor(rate) &&
        isfinite(frequency) && frequency >= LIME_RX_MIN_FREQUENCY && frequency <= LIME_RX_MAX_FREQUENCY &&
        isfinite(gain) && gain >= 0 && gain <= LIME_RX_MAX_GAIN && gain == floor(gain);
}
typedef struct LimeRx LimeRx;
LimeRx *lime_rx_open(const char *args, const char *antenna, double rate, double frequency, double *gain, double *actual);
int lime_rx_start(LimeRx *r);
int lime_rx_frequency(LimeRx *r, double frequency);
int lime_rx_gain(LimeRx *r, double *gain);
int lime_rx_read(LimeRx *r, int16_t *iq, size_t samples);
int lime_rx_report(LimeRx *r);
void lime_rx_close(LimeRx *r);
#endif
