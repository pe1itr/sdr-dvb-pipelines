#ifndef USB_RECEIVER_H
#define USB_RECEIVER_H
#include <glib.h>
#include "usb_demod.h"
typedef struct UsbReceiver UsbReceiver;
UsbReceiver *usb_receiver_new(GError **error);
void usb_receiver_free(UsbReceiver *r);
const char *usb_receiver_tap_path(UsbReceiver *r);
gboolean usb_receiver_start(UsbReceiver *r, const char *bind_host, unsigned port, const char *device, GError **error);
void usb_receiver_stop(UsbReceiver *r);
gboolean usb_receiver_running(UsbReceiver *r);
gboolean usb_receiver_set_mode(UsbReceiver *r, MonitorMode mode);
MonitorMode usb_receiver_mode(UsbReceiver *r);
void usb_receiver_set_tci_level(UsbReceiver *r, int db);
void usb_receiver_set_muted(UsbReceiver *r, gboolean muted);
gboolean usb_receiver_muted(UsbReceiver *r);
/* Raw mono s16le, 48 kHz, 480 samples per UDP datagram. Independent of mute.
 * Can be configured while running; a rejected update preserves the old output. */
gboolean usb_receiver_set_pcm_udp(UsbReceiver *r, gboolean enabled, const char *host, unsigned port, GError **error);
gboolean usb_receiver_pcm_udp_enabled(UsbReceiver *r);
void usb_receiver_source(UsbReceiver *r, double center, double rate, gboolean active);
gboolean usb_receiver_tune(UsbReceiver *r, double frequency);
double usb_receiver_frequency(UsbReceiver *r);
void usb_receiver_status(UsbReceiver *r, char *message, size_t size);
#endif
