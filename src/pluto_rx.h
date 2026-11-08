#ifndef PLUTO_RX_H
#define PLUTO_RX_H
#include <stddef.h>
typedef struct { float i, q; } PlutoIQ;
typedef struct {
    const char *pluto_uri, *ref;
    unsigned channel;
    double rate, bandwidth, frequency, gain;
} PlutoConfig;
typedef struct PlutoSource PlutoSource;
PlutoSource *pluto_source_open(PlutoConfig *c);
int pluto_source_tune(PlutoSource *s, double *hz, double *gain);
int pluto_source_read(PlutoSource *s, PlutoIQ *iq, size_t n);
void pluto_source_stop(PlutoSource *s);
void pluto_source_close(PlutoSource *s);
#endif
