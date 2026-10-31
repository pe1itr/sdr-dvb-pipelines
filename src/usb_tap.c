#define _POSIX_C_SOURCE 200809L
#include "usb_tap.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
void usb_tap_feed(const unsigned char *iq, size_t samples, int u8, double rate) {
    static int initialized;
    static UsbTap *tap;
    static uint64_t sequence;
    if (!initialized) {
        initialized=1;
        const char *path=getenv("LINRAD_USB_TAP");
        if (!path || !*path) return;
        int fd=open(path,O_RDWR | O_CLOEXEC);
        struct stat st;
        if (fd<0) return;
        if (!fstat(fd,&st) && st.st_size==(off_t)sizeof(UsbTap)) {
            void *p=mmap(NULL,sizeof(UsbTap),PROT_READ | PROT_WRITE,MAP_SHARED,fd,0);
            if (p!=MAP_FAILED) {
                tap=p;
                if (tap->magic!=USB_TAP_MAGIC) { munmap(tap,sizeof(*tap)); tap=NULL; }
            }
        }
        close(fd);
    }
    if (!tap || !atomic_load_explicit(&tap->enabled,memory_order_relaxed)) return;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
    size_t stride=u8 ? 2 : 4;
    while (samples) {
        unsigned n=samples>USB_TAP_SAMPLES ? USB_TAP_SAMPLES : (unsigned)samples;
        unsigned w=atomic_load_explicit(&tap->write_index,memory_order_relaxed);
        unsigned r=atomic_load_explicit(&tap->read_index,memory_order_acquire);
        ++sequence;
        if (w-r>=USB_TAP_SLOTS) {
            atomic_fetch_add_explicit(&tap->dropped,1,memory_order_relaxed);
        } else {
            UsbTapBlock *b=&tap->blocks[w%USB_TAP_SLOTS];
            b->rate=rate; b->samples=n; b->u8=!!u8; b->sequence=sequence;
            b->time_us=(int64_t)now.tv_sec*1000000+now.tv_nsec/1000;
            memcpy(b->iq,iq,n*stride);
            atomic_store_explicit(&tap->write_index,w+1,memory_order_release);
        }
        iq+=n*stride; samples-=n;
    }
}
