#define _POSIX_C_SOURCE 200809L
#include "usb_receiver.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
static volatile sig_atomic_t stopping;
static void stop(int sig) { (void)sig; stopping=1; }
/* Use the real USB demodulator/TCI/PCM path with an external IQ producer.
 * ALSA null keeps this regression independent of a user's speakers. */
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    GError *error=NULL;
    UsbReceiver *r=usb_receiver_new(&error);
    if (!r) return 1;
    usb_receiver_source(r,28000000,atof(argv[1]),TRUE);
    if (!usb_receiver_start(r,"127.0.0.1",atoi(argv[2]),"null",&error) ||
        !usb_receiver_set_pcm_udp(r,TRUE,"127.0.0.1",atoi(argv[3]),&error)) {
        fprintf(stderr,"%s\n",error->message); usb_receiver_free(r); return 1;
    }
    signal(SIGTERM,stop); signal(SIGINT,stop);
    puts(usb_receiver_tap_path(r)); fflush(stdout);
    while (!stopping) g_usleep(10000);
    char status[512]; usb_receiver_status(r,status,sizeof(status));
    fprintf(stderr,"%s\n",status);
    usb_receiver_free(r); return 0;
}
