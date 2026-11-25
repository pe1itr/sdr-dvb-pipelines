// SPDX-License-Identifier: GPL-3.0-only
// FT2232H A controls the NIM, B delivers the transport stream.
#include "monitor.h"
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <libusb.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t stopping;
static void stop(int signal_number) {
    (void)signal_number;
    stopping = 1;
}

static int find_receiver(libusb_context *context, libusb_device_handle **result) {
    libusb_device **devices;
    ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0) return (int)count;
    int status = LIBUSB_ERROR_NO_DEVICE;
    for (ssize_t i = 0; i < count; ++i) {
        struct libusb_device_descriptor info;
        if (libusb_get_device_descriptor(devices[i], &info) ||
            info.idVendor != 0x0403 || info.idProduct != 0x6010) continue;
        libusb_device_handle *candidate;
        int opened = libusb_open(devices[i], &candidate);
        if (opened < 0) {
            if (!*result) status = opened;
            continue;
        }
        unsigned char product[128];
        int n = libusb_get_string_descriptor_ascii(candidate, info.iProduct, product, sizeof(product));
        if (n > 0 && !strcmp((const char *)product, "MiniTiouner-Express")) {
            if (*result) {
                fprintf(stderr, "Multiple MiniTiouner-Express receivers connected\n");
                libusb_close(candidate);
                status = LIBUSB_ERROR_BUSY;
                break;
            }
            *result = candidate;
            status = 0;
        } else libusb_close(candidate);
    }
    libusb_free_device_list(devices, 1);
    return status;
}

static int receive(libusb_device_handle *device) {
    unsigned char buffer[32768];
    int size = libusb_get_max_packet_size(libusb_get_device(device), 0x83);
    if (size < 3) return LIBUSB_ERROR_IO;
    while (!stopping) {
        int length = 0;
        int rc = libusb_bulk_transfer(device, 0x83, buffer, sizeof(buffer), &length, 250);
        if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT && rc != LIBUSB_ERROR_INTERRUPTED) return rc;
        // Every FTDI USB packet starts with two modem-status bytes, including
        // short packets. MPEG-TS boundaries need not match USB boundaries.
        int used = 0;
        for (int pos = 0; pos < length; pos += size) {
            int bytes = length - pos < size ? length - pos : size;
            if (bytes < 2) return LIBUSB_ERROR_IO;
            memmove(buffer + used, buffer + pos + 2, bytes - 2);
            used += bytes - 2;
        }
        if (used && fwrite(buffer, 1, used, stdout) != (size_t)used) return LIBUSB_ERROR_IO;
    }
    return fflush(stdout) ? LIBUSB_ERROR_IO : 0;
}

static bool number(const char *text, unsigned *value) {
    char *end;
    errno=0;
    unsigned long v=strtoul(text,&end,10);
    if (!*text || *text=='-' || *end || errno || v>UINT_MAX) return false;
    *value=v;
    return true;
}

int main(int argc, char **argv) {
    MtTune tune={.detector2=2,.alpha=-1,.beta=-1,.lock_acquire=-1,.lock_release=-1};
    unsigned dx_relock=0;
    if ((argc!=7 && argc!=11 && argc!=12 && argc!=14) || !number(argv[1],&tune.frequency_khz) || !number(argv[2],&tune.symbol_rate) ||
        !number(argv[3],&tune.standard) || !number(argv[4],&tune.code) ||
        !number(argv[5],&tune.input_b) || !number(argv[6],&tune.search_khz)) {
        fprintf(stderr,"Usage: mt_express_rx frequency_kHz symbol_rate standard(1|2) code input_B(0|1) search_kHz [alpha beta lock_acquire lock_release [dx_relock [detector1 detector2]]]\n");
        return 2;
    }
    if (argc==14 && (!number(argv[12],&tune.detector1) || tune.detector1>2 ||
                     !number(argv[13],&tune.detector2) || tune.detector2>2)) return 2;
    if (argc>=12 && (!number(argv[11],&dx_relock) || dx_relock>1)) return 2;
    if (argc>=11) {
        int *settings[]={&tune.alpha,&tune.beta,&tune.lock_acquire,&tune.lock_release};
        for (int i=0;i<4;++i) {
            unsigned value;
            if (!strcmp(argv[7+i],"-1")) continue;
            if (!number(argv[7+i],&value) || value>(i<2 ? 63U : 255U)) return 2;
            *settings[i]=value;
        }
    }
    libusb_context *context = NULL;
    libusb_device_handle *device = NULL;
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    signal(SIGPIPE, SIG_IGN);
    int rc = libusb_init(&context);
    if (!rc) rc = find_receiver(context, &device);
    if (!rc) rc = libusb_set_auto_detach_kernel_driver(device, 1);
    bool control=false, ts=false, monitoring=false;
    pthread_t thread;
    MtMonitor monitor={.device=device,.socket=-1,.dx_relock=dx_relock,.input_b=tune.input_b,
                       .master_clock_hz=mt_master_clock_hz(tune.symbol_rate),.tune=tune,
                       .live_tuning=getenv("LINRAD_MT_TUNE_PIPE")!=NULL};
    if (!rc && monitor.live_tuning && fcntl(STDIN_FILENO,F_SETFL,O_NONBLOCK)<0) rc=LIBUSB_ERROR_IO;
    if (!rc) { rc=mt_control_open(device); control=!rc; }
    if (!rc) { rc=libusb_claim_interface(device,1); ts=!rc; }
    if (!rc) rc=mt_tune(device,&tune);
    const char *port=getenv("LINRAD_DECODER_MONITOR_PORT");
    if (!rc && port) {
        unsigned n;
        if (!number(port,&n) || !n || n>65535) rc=LIBUSB_ERROR_INVALID_PARAM;
        else {
            struct sockaddr_in dest={.sin_family=AF_INET,.sin_port=htons(n),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
            monitor.socket=socket(AF_INET,SOCK_DGRAM,0);
            if (monitor.socket<0 || connect(monitor.socket,(struct sockaddr *)&dest,sizeof(dest))<0) rc=LIBUSB_ERROR_OTHER;
        }
    }
    const char *scan_port=getenv("LINRAD_MT_SCAN_PORT");
    if (scan_port && number(scan_port,&monitor.scan_port) && monitor.scan_port<=65535)
        monitor.scan_enabled=getenv("LINRAD_MT_SCAN_ON")!=NULL;
    else monitor.scan_port=0;
    if (!rc && (port || dx_relock || monitor.live_tuning)) {
        if (pthread_create(&thread,NULL,mt_monitor_run,&monitor)) rc=LIBUSB_ERROR_OTHER;
        else monitoring=true;
    }
    if (!rc) {
        fprintf(stderr,"MT Express: input %c, %u kHz, %u sym/s, DVB-S%s, demod clock %u Hz, LNB off\n",tune.input_b ? 'B' : 'A', tune.frequency_khz,tune.symbol_rate,tune.standard==2 ? "2" : "",monitor.master_clock_hz);
        rc = receive(device);

    }
    atomic_store(&monitor.finished,true);
    if (monitoring) pthread_join(thread,NULL);
    if (!rc && monitor.live_tuning && monitor.error) rc=monitor.error;
    if (monitor.socket>=0) close(monitor.socket);
    if (ts) libusb_release_interface(device,1);
    if (control) libusb_release_interface(device,0);
    if (device) libusb_close(device);
    if (context) libusb_exit(context);
    if (rc) fprintf(stderr, "MT Express: %s\n", libusb_error_name(rc));
    return rc != 0;
}
