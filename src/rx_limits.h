#ifndef RX_LIMITS_H
#define RX_LIMITS_H
#include <math.h>
#include <string.h>

/* Limits apply to physical SDR/IF frequencies, before converter LO labels.
 * HF+ is deliberately the Dual Port VHF input (manufacturer: 64..260 MHz).
 * RSP1A/B: 1 kHz..2 GHz. See the linked specifications in README.md. */
static inline int rx_rsp(const char *source) {
    return !strcmp(source, "rsp1a") || !strcmp(source, "rsp1b");
}
static inline int rx_airspyhf(const char *source) { return !strcmp(source, "airspyhf"); }
static inline int rx_airspyr2(const char *source) { return !strcmp(source, "airspyr2"); }
static inline int rx_airspyr2_rate_valid(double rate) { return rate == 2500000 || rate == 10000000; }
static inline double rx_frequency_min(const char *source) {
    if (rx_airspyr2(source)) return 24000000;
    if (rx_airspyhf(source)) return 64000000;
    if (rx_rsp(source)) return 1000;
    if (!strcmp(source,"lime")) return 10000000;
    if (!strcmp(source,"fcdpp")) return 150000;
    if (!strcmp(source,"rtlsdr")) return 24000000;
    if (!strcmp(source,"afedri")) return 0;
    if (!strcmp(source,"sdriq")) return 500;
    return !strcmp(source,"hackrf") ? 1000000 : 70000000;
}
static inline double rx_frequency_max(const char *source) {
    if (rx_airspyr2(source)) return 1700000000;
    if (rx_airspyhf(source)) return 260000000;
    if (rx_rsp(source)) return 2000000000;
    if (!strcmp(source,"lime")) return 3500000000.0;
    if (!strcmp(source,"fcdpp")) return 1900000000;
    if (!strcmp(source,"rtlsdr")) return 1764000000;
    if (!strcmp(source,"afedri")) return 35000000;
    if (!strcmp(source,"sdriq")) return 30000000;
    return 6000000000.0;
}
static inline int rx_frequency_valid(const char *source, double frequency) {
    return isfinite(frequency) && frequency >= rx_frequency_min(source) &&
        frequency <= rx_frequency_max(source) &&
        (strcmp(source,"fcdpp") || frequency <= 240000000 || frequency >= 420000000);
}
static inline int rx_tuning_valid(const char *source, double frequency, double offset) {
    return rx_frequency_valid(source,frequency) && rx_frequency_valid(source,frequency+offset);
}
/* Reserve 2.5% at each sample-band edge. The HF+ additionally has a
 * specified 660 kHz usable passband at its normal 768 kS/s rate. */
static inline double rx_usable_bandwidth(const char *source, double rate) {
    double usable=rate*.95;
    /* R2 specifies 9 MHz alias/image-free at 10 MS/s. Use a conservative
     * 90% passband also for the experimental 2.5 MS/s mode. */
    if (rx_airspyr2(source)) return rate*.90;
    if (rx_airspyhf(source)) return fmin(usable,660000);
    /* SoapySDRPlay3 selects the largest supported analog filter <= rate
     * (200 kHz minimum). Account for it as well as the output sample rate. */
    if (rx_rsp(source)) {
        double filter=rate < 300000 ? 200000 : rate < 600000 ? 300000 :
            rate < 1536000 ? 600000 : rate < 5000000 ? 1536000 :
            rate < 6000000 ? 5000000 : rate < 7000000 ? 6000000 :
            rate < 8000000 ? 7000000 : 8000000;
        return fmin(usable,filter);
    }
    return usable;
}
static inline int rx_bandwidth_fits(const char *source, double rate, double offset, double width) {
    return isfinite(rate) && rate > 0 && isfinite(offset) && isfinite(width) && width >= 0 &&
        2*fabs(offset)+width <= rx_usable_bandwidth(source,rate);
}
#endif
