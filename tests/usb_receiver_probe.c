#define _POSIX_C_SOURCE 200809L
#include "usb_receiver.h"
#include "usb_tap.h"
#include <math.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static volatile sig_atomic_t stopping;
static void stop(int sig) { (void)sig; stopping=1; }
int main(int argc, char **argv) {
    if (argc<2 || argc>4) return 2;
    unsigned pcm_port=argc>=3 ? atoi(argv[2]) : 0;
    FILE *recording=argc==4 ? fopen(argv[3],"rb") : NULL;
    if (argc==4 && !recording) return 2;
    gboolean playback=FALSE;
    GError *error=NULL;
    UsbReceiver *r=usb_receiver_new(&error);
    if (!r) { fprintf(stderr,"%s\n",error->message); return 1; }
    usb_receiver_source(r,144600000,48000,TRUE);
    if (!usb_receiver_start(r,"127.0.0.1",atoi(argv[1]),"null",&error)) {
        fprintf(stderr,"%s\n",error->message); usb_receiver_free(r); return 1;
    }
    setenv("LINRAD_USB_TAP",usb_receiver_tap_path(r),1);
    signal(SIGTERM,stop); signal(SIGINT,stop);
    fcntl(STDIN_FILENO,F_SETFL,O_NONBLOCK);
    puts("ready"); fflush(stdout);
    unsigned char iq[480*4]; uint64_t sample=0;
    gint64 deadline=g_get_monotonic_time();
    while (!stopping) {
        char command;
        while (read(STDIN_FILENO,&command,1)==1) {
            if (command=='f' || command=='u' || command=='d')
                usb_receiver_set_mode(r,command=='d' ? MONITOR_FM_DATA : command=='f' ? MONITOR_FM : MONITOR_USB);
            if (command=='l') usb_receiver_set_tci_level(r,-18);
            if (command=='g') usb_receiver_set_tci_level(r,0);
            if (command=='t') usb_receiver_set_tci_only(r,TRUE);
            if (command=='m') usb_receiver_set_muted(r,TRUE);
            if (command=='a') usb_receiver_set_muted(r,FALSE);
            if (command=='e' || command=='x') {
                if (!usb_receiver_set_pcm_udp(r,command=='e',"127.0.0.1",pcm_port,&error)) {
                    fprintf(stderr,"%s\n",error->message); return 1;
                }
            }
            if (command=='v') {
                gboolean before=usb_receiver_pcm_udp_enabled(r);
                const char *hosts[]={"bad address","0.0.0.0","255.255.255.255","127.0.0.1","127.0.0.1"};
                unsigned ports[]={7355,7355,7355,0,65536};
                for (unsigned j=0;j<G_N_ELEMENTS(hosts);++j) {
                    g_assert_false(usb_receiver_set_pcm_udp(r,TRUE,hosts[j],ports[j],&error));
                    g_assert_nonnull(error); g_clear_error(&error);
                    g_assert_cmpint(usb_receiver_pcm_udp_enabled(r),==,before);
                }
            }
            if (command=='s') usb_receiver_stop(r);
            if (command=='r' && !usb_receiver_start(r,"127.0.0.1",atoi(argv[1]),"null",&error)) return 1;
            if (command=='i' && recording) { rewind(recording); playback=TRUE; }
            printf("%c\n",command); fflush(stdout);
        }
        MonitorMode mode=usb_receiver_mode(r);
        for (unsigned i=0;i<480;++i,++sample) {
            double phase=6.283185307179586*1000*sample/48000;
            if (mode!=MONITOR_USB) phase=2.5*sin(phase);
            int16_t re=lrint(8000*cos(phase)), im=lrint(8000*sin(phase));
            iq[i*4]=re&255; iq[i*4+1]=(uint16_t)re>>8;
            iq[i*4+2]=im&255; iq[i*4+3]=(uint16_t)im>>8;
        }
        if (playback) {
            size_t count=fread(iq,4,480,recording);
            if (count<480) { memset(iq+count*4,0,(480-count)*4); playback=FALSE; }
        }
        usb_tap_feed(iq,480,0,48000);
        deadline+=10000;
        gint64 delay=deadline-g_get_monotonic_time(); if (delay>0) g_usleep(delay);
    }
    if (recording) fclose(recording);
    usb_receiver_free(r); return 0;
}
