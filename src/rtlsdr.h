#ifndef LINRAD_RTLSDR_H
#define LINRAD_RTLSDR_H
#include <math.h>

/* Normal tuner mode for R820T/R820T2/R828D; no direct sampling or bias tee. */
#define RTLSDR_MIN_FREQUENCY 24000000
#define RTLSDR_MAX_FREQUENCY 1764000000
#define RTLSDR_MAX_GAIN 49.6
static inline int rtlsdr_rate_valid(double rate) {
    return isfinite(rate) && rate == floor(rate) &&
        ((rate >= 225001 && rate <= 300000) || (rate >= 900001 && rate <= 3200000));
}
static const int rtlsdr_rates[] = {
    250000, 1024000, 1200000, 1536000, 1792000, 1920000,
    2048000, 2400000, 2560000, 2880000, 3200000
};
#endif
