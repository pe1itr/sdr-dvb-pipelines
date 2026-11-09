#ifndef HACKRF_GAIN_H
#define HACKRF_GAIN_H

/* Gain excludes the separately controlled RF AMP. Use only hardware steps:
 * LNA first at a fixed VGA floor, then a bounded VGA increase. */
enum { HACKRF_VGA_BASE = 12, HACKRF_VGA_LIMIT = 24,
       HACKRF_LNA_LIMIT = 40, HACKRF_GAIN_DEFAULT = 44,
       HACKRF_GAIN_MAX = HACKRF_LNA_LIMIT + HACKRF_VGA_LIMIT };
typedef struct { int lna, vga; } HackrfGain;

static inline HackrfGain hackrf_gain(double requested) {
    if (!(requested >= HACKRF_VGA_BASE)) requested = HACKRF_VGA_BASE;
    if (requested > HACKRF_GAIN_MAX) requested = HACKRF_GAIN_MAX;
    int extra = (int)requested - HACKRF_VGA_BASE;
    HackrfGain g = {extra < HACKRF_LNA_LIMIT ? extra / 8 * 8 : HACKRF_LNA_LIMIT,
        HACKRF_VGA_BASE + (extra > HACKRF_LNA_LIMIT ? (extra - HACKRF_LNA_LIMIT) / 2 * 2 : 0)};
    return g;
}

static inline int hackrf_gain_step(double current, int direction) {
    HackrfGain g = hackrf_gain(current);
    int total = g.lna + g.vga;
    int step = direction > 0 ? (g.lna < HACKRF_LNA_LIMIT ? 8 : 2)
        : (g.vga > HACKRF_VGA_BASE ? 2 : 8);
    int next = total + (direction > 0 ? step : -step);
    return next < HACKRF_VGA_BASE ? HACKRF_VGA_BASE : next > HACKRF_GAIN_MAX ? HACKRF_GAIN_MAX : next;
}
#endif
