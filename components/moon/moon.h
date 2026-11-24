#ifndef LINRAD_MOON_H
#define LINRAD_MOON_H
#include "../../src/iq_shift.h"
typedef struct { double latitude, longitude; } MoonSite;
typedef struct { double azimuth, elevation, doppler, distance_km; } MoonLeg;
typedef struct { MoonLeg rx, dx; double doppler; } MoonPrediction;
int moon_predict(double unix_utc, double tx_hz, const MoonSite *rx, const MoonSite *dx, MoonPrediction *prediction);

/* --moon-track RX_GRID,DX_GRID,TX_HZ,IQ_CENTER_RF_HZ,CHANNEL_WIDTH_HZ */
typedef struct {
    MoonSite rx, dx;
    double tx, center, width, next_update;
} MoonTracker;
int moon_tracker_parse(MoonTracker *tracker, const char *spec);
int moon_tracker_update(MoonTracker *tracker, IQShift *shift, double rate, double sample_utc);
#endif
