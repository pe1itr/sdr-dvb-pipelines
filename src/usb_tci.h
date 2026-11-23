#ifndef USB_TCI_H
#define USB_TCI_H
#include <glib.h>
#include "usb_demod.h"
typedef struct UsbTci UsbTci;
typedef double (*UsbTciTune)(double frequency, void *user);
UsbTci *usb_tci_new(const char *host, unsigned port, UsbTciTune tune, void *user, GError **error);
void usb_tci_free(UsbTci *s);
void usb_tci_poll(UsbTci *s);
void usb_tci_frequency(UsbTci *s, double frequency);
void usb_tci_mode(UsbTci *s, MonitorMode mode);
void usb_tci_audio(UsbTci *s, const float *samples, size_t count, float gain);
unsigned usb_tci_clients(UsbTci *s);
#endif
