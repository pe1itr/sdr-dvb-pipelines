#ifndef TUNE_CONTROL_H
#define TUNE_CONTROL_H
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { char line[80]; unsigned used; int discard; } TuneControl;
/* Nonblocking lines: frequency in Hz, or "gain DB". Return bit 1 for
 * frequency, bit 2 for gain, bit 4 for "bandwidth HZ" (0 = bypass). Keep the latest complete valid
 * request; bound work so keyboard repeat cannot starve reception. */
static inline int tune_control_read(TuneControl *c, int fd, double low, double high, double *frequency,
        double gain_low, double gain_high, double gain_step, double *gain,
        double sample_rate, double *bandwidth) {
    int changed = 0;
    for (int i = 0; i < 4096; ++i) {
        char ch;
        ssize_t n = read(fd, &ch, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (ch == '\n') {
            c->line[c->used] = 0;
            char *end;
            errno = 0;
            int is_gain = !strncmp(c->line, "gain ", 5);
            int is_filter = !strncmp(c->line, "bandwidth ", 10);
            char *number = c->line + (is_filter ? 10 : is_gain ? 5 : 0);
            double value = strtod(number, &end);
            if (!c->discard && !errno && end != number && !*end && isfinite(value)) {
                if (is_filter) {
                    if (value == 0 || (value >= fmax(10, sample_rate/256) && value <= sample_rate*.95)) {
                        *bandwidth = value; changed |= 4;
                    }
                } else if (is_gain) {
                    if (value >= gain_low && value <= gain_high &&
                        (gain_step <= 0 || fmod(value-gain_low, gain_step) == 0)) {
                        *gain = value; changed |= 2;
                    }
                } else if (value >= low && value <= high) {
                    *frequency = value; changed |= 1;
                }
            }
            c->used = 0; c->discard = 0;
        } else if (c->used < sizeof(c->line)-1) c->line[c->used++] = ch;
        else c->discard = 1;
    }
    return changed;
}
#endif
