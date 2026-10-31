#ifndef USB_TAP_H
#define USB_TAP_H
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
/* Single producer/single consumer. A full queue drops USB samples; it never
 * waits for audio, the GUI, or a network client. DVB retains the original IQ. */
#define USB_TAP_SAMPLES 8192
#define USB_TAP_SLOTS 128
#define USB_TAP_MAGIC 0x55534231u
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "USB tap requires lock-free shared atomics");
typedef struct {
    double rate;
    uint64_t sequence;
    int64_t time_us;
    unsigned samples, u8;
    unsigned char iq[USB_TAP_SAMPLES * 4];
} UsbTapBlock;
typedef struct {
    unsigned magic;
    atomic_uint enabled, write_index, read_index, dropped;
    UsbTapBlock blocks[USB_TAP_SLOTS];
} UsbTap;
void usb_tap_feed(const unsigned char *iq, size_t samples, int u8, double rate);
#endif
