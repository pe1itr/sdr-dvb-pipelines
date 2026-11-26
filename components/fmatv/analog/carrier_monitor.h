#ifndef FMATV_CARRIER_MONITOR_H
#define FMATV_CARRIER_MONITOR_H
#include <stddef.h>
typedef struct CarrierMonitor CarrierMonitor;
CarrierMonitor *carrier_monitor_create(double rate, int stereo);
void carrier_monitor_feed(void *monitor, const float *samples, size_t count);
unsigned carrier_monitor_detected(CarrierMonitor *monitor);
void carrier_monitor_destroy(CarrierMonitor *monitor);
#endif
