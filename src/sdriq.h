#ifndef SDRIQ_H
#define SDRIQ_H

/* Rounded output rates and RF ATT steps used by the RFspace SDR-IQ driver. */
static const int sdriq_rates[] = {8138, 16276, 37793, 55556, 111111, 158730, 196078};
static inline int sdriq_rate_valid(double rate) {
    for (unsigned i = 0; i < sizeof(sdriq_rates)/sizeof(sdriq_rates[0]); ++i)
        if (rate == sdriq_rates[i]) return 1;
    return 0;
}
static inline int sdriq_gain_valid(double gain) {
    return gain == -20 || gain == -10 || gain == 0 || gain == 10;
}
#endif
