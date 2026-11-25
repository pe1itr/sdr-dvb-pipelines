#include "usb_demod.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PI 3.14159265358979323846
static void output(const float *audio, size_t count, void *user) {
    (void)user;
    if (fwrite(audio,sizeof(float),count,stdout)!=count) exit(2);
}
int main(int argc, char **argv) {
    if ((argc==2 && !strcmp(argv[1],"fm-data-iq")) || (argc==5 && !strcmp(argv[1],"iq"))) {
        MonitorMode mode=argc==2 || !strcmp(argv[2],"fm-data") ? MONITOR_FM_DATA :
            !strcmp(argv[2],"fm") ? MONITOR_FM : MONITOR_USB;
        UsbDemod *d=usb_demod_new_mode(argc==2 ? 48000 : atoi(argv[4]),0,mode);
        if (!d) return 1;
        usb_demod_set_fm_squelch(d,argc==5 && atoi(argv[3]));
        unsigned char iq[8192*4]; size_t count;
        while ((count=fread(iq,4,8192,stdin))) usb_demod_feed(d,iq,count,0,output,NULL);
        usb_demod_free(d); return ferror(stdin) ? 2 : 0;
    }
    if (argc<4 || argc>6) return 2;
    double rate=atof(argv[1]), offset=atof(argv[2]); int u8=atoi(argv[3]);
    int fm=argc>=5;
    MonitorMode mode=!fm ? MONITOR_USB : !strcmp(argv[4],"fm-data") ? MONITOR_FM_DATA : MONITOR_FM;
    double audio_hz=argc==6 ? atof(argv[5]) : 1000;
    UsbDemod *d=usb_demod_new_mode(rate,offset,mode);
    if (!d) return 1;
    unsigned char iq[8192*4];
    /* Desired USB tone plus equal-strength lower-sideband, out-of-band, and
     * first CIC alias tones. All must be separated by the real receiver. */
    double tones[]={offset+1000,offset-1800,offset+5000,
        offset+1500+rate/fmax(1,floor(rate/24000))+800};
    uint64_t total=(uint64_t)(rate*1.2);
    for (uint64_t start=0;start<total;start+=8192) {
        size_t count=total-start>8192 ? 8192 : total-start;
        for (size_t i=0;i<count;++i) {
            double re=0,im=0;
            for (unsigned t=0;t<(rate>=48000 ? 4u : 3u);++t) {
                double phase=2*PI*tones[t]*(start+i)/rate;
                re+=.12*cos(phase); im+=.12*sin(phase);
            }
            if (fm) {
                double t=(start+i)/rate;
                double carrier=offset+(mode==MONITOR_FM_DATA && audio_hz==0 ? 500 : 0);
                double phase=2*PI*carrier*t+(audio_hz>0 ? 2500/audio_hz*sin(2*PI*audio_hz*t) : 0);
                double amplitude=.35*(1+.5*sin(2*PI*1800*t));
                re=audio_hz<0 ? 0 : amplitude*cos(phase);
                im=audio_hz<0 ? 0 : amplitude*sin(phase);
                if (audio_hz>0 && rate>=48000) {
                    phase=2*PI*(offset+(mode==MONITOR_FM_DATA ? 20000 : 12500))*t+1.25*sin(2*PI*2000*t);
                    re+=.2*cos(phase); im+=.2*sin(phase);
                }
            }
            if (u8) { iq[i*2]=(unsigned char)lrint(128+re*127); iq[i*2+1]=(unsigned char)lrint(128+im*127); }
            else {
                int16_t a=lrint(re*32767), b=lrint(im*32767);
                iq[i*4]=a&255; iq[i*4+1]=(uint16_t)a>>8;
                iq[i*4+2]=b&255; iq[i*4+3]=(uint16_t)b>>8;
            }
        }
        usb_demod_feed(d,iq,count,u8,output,NULL);
    }
    usb_demod_free(d); return 0;
}
