#ifndef USB_DEMOD_H
#define USB_DEMOD_H
#include <stddef.h>
typedef enum { MONITOR_USB, MONITOR_FM, MONITOR_FM_DATA } MonitorMode;
static inline double monitor_low(MonitorMode mode) { return mode==MONITOR_FM_DATA ? -10000 : mode==MONITOR_FM ? -6250 : 0; }
static inline double monitor_high(MonitorMode mode) { return mode==MONITOR_FM_DATA ? 10000 : mode==MONITOR_FM ? 6250 : 3000; }
static inline double monitor_min_rate(MonitorMode mode) { return mode==MONITOR_FM_DATA ? 24000 : mode==MONITOR_FM ? 12500 : 8000; }
static inline const char *monitor_name(MonitorMode mode) { return mode==MONITOR_FM_DATA ? "FM-DATA" : mode==MONITOR_FM ? "FM" : "USB"; }
typedef struct UsbDemod UsbDemod;
typedef void (*UsbAudioCallback)(const float *audio, size_t count, void *user);
UsbDemod *usb_demod_new(double rate, double offset);
UsbDemod *usb_demod_new_mode(double rate, double offset, MonitorMode mode);
void usb_demod_set_fm_squelch(UsbDemod *d, int enabled);
void usb_demod_free(UsbDemod *d);
void usb_demod_feed(UsbDemod *d, const unsigned char *iq, size_t samples, int u8,
                    UsbAudioCallback callback, void *user);
#endif
