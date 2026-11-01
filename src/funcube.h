#ifndef FUNCUBE_H
#define FUNCUBE_H
#include <stddef.h>
#include <stdint.h>
#include <math.h>

#define FUNCUBE_RATE 192000
#define FUNCUBE_MIN_FREQUENCY 150000
#define FUNCUBE_MAX_FREQUENCY 1900000000
#define FUNCUBE_MAX_GAIN 59
static const int funcube_rates[] = {FUNCUBE_RATE};
static inline int funcube_frequency_valid(double frequency) {
    return (frequency >= FUNCUBE_MIN_FREQUENCY && frequency <= 240000000) ||
           (frequency >= 420000000 && frequency <= FUNCUBE_MAX_FREQUENCY);
}
static inline int funcube_gain_valid(double gain) {
    return gain >= 0 && gain <= FUNCUBE_MAX_GAIN && gain == floor(gain);
}
typedef struct Funcube Funcube;
/* Linux hidraw control and the matching ALSA hardware capture device. */
Funcube *funcube_open(const char *args, double frequency, double gain);
int funcube_frequency(Funcube *f, double frequency);
int funcube_gain(Funcube *f, double gain);
int funcube_read(Funcube *f, int16_t *iq, size_t samples);
void funcube_close(Funcube *f);
#endif
