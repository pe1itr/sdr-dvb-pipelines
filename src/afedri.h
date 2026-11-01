#ifndef AFEDRI_H
#define AFEDRI_H
#include <stdint.h>
#include <stddef.h>

typedef struct Afedri Afedri;
/* Single-channel, long UDP packets, signed 16-bit I,Q. No persistent settings. */
Afedri *afedri_open(const char *ip, int port, const char *bind_ip,
                    double rate, double frequency, double gain, double *actual_rate);
int afedri_start(Afedri *sdr);
int afedri_frequency(Afedri *sdr, double hz);
int afedri_gain(Afedri *sdr, double db);
/* Aggregate packets for DSP; capacity must be a multiple of 256 samples.
 * Partial blocks flush within 10 ms, even if the stream stops. */
int afedri_read(Afedri *sdr, int16_t *iq, size_t capacity);
void afedri_report(Afedri *sdr);
void afedri_close(Afedri *sdr);
static inline int afedri_gain_valid(double db) {
    if (!(db >= -10 && db <= 35)) return 0;
    int step = (int)((db + 10) / 3);
    return db == -10 + 3 * step;
}
#endif
