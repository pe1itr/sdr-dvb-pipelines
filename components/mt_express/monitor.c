// SPDX-License-Identifier: GPL-3.0-only
#include "monitor.h"
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include "../../src/decoder_monitor.h"
#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <time.h>

static const char *s2_code[]={"","1/4","1/3","2/5","1/2","3/5","2/3","3/4","4/5","5/6","8/9","9/10",
    "3/5","2/3","3/4","5/6","8/9","9/10","2/3","3/4","4/5","5/6","8/9","9/10","3/4","4/5","5/6","8/9","9/10"};

// Decision-directed MER over a complete window of tapped symbols. Fit one
// complex gain for the whole window, retaining radial and phase noise.
enum { mer_window=256 };
typedef struct {
    float iq[2*mer_window];
    unsigned used, order;
    int code;
    double db;
} MerWindow;

static double constellation_mer(const MerWindow *w) {
    const double pi=acos(-1);
    unsigned rings=1, n[]={w->order,12,16};
    double radius[]={1,1,1}, offset[]={w->order==4 ? pi/4 : 0,pi/12,0};
    // ETSI EN 302 307-1 section 5.4, linear-channel radii.
    if (w->order==16 || w->order==32) {
        static const double middle[]={3.15,2.85,2.75,2.70,2.60,2.57,2.84,2.72,2.64,2.54,2.53};
        static const double outer[]={5.27,4.87,4.64,4.33,4.30};
        n[0]=4; offset[0]=pi/4;
        rings=w->order==16 ? 2 : 3;
        radius[1]=middle[w->code-18];
        if (rings==3) radius[2]=outer[w->code-24];
    }
    double re[32],im[32],ref_power=0,rx_power=0;
    unsigned count=0;
    for (unsigned r=0;r<rings;r++) for (unsigned k=0;k<n[r];k++) {
        double angle=offset[r]+2*pi*k/n[r];
        re[count]=radius[r]*cos(angle);
        im[count++]=radius[r]*sin(angle);
        ref_power+=radius[r]*radius[r];
    }
    for (unsigned k=0;k<2*mer_window;k++) rx_power+=w->iq[k]*w->iq[k];
    double gr=sqrt(rx_power*count/(mer_window*ref_power)),gi=0;
    double signal=0,error=0;
    for (int pass=0;pass<3;pass++) {
        double cross_r=0,cross_i=0,energy=0;
        signal=error=0;
        for (unsigned k=0;k<mer_window;k++) {
            double x=w->iq[2*k],y=w->iq[2*k+1],best=INFINITY;
            unsigned nearest=0;
            for (unsigned j=0;j<count;j++) {
                double dx=x-(gr*re[j]-gi*im[j]),dy=y-(gr*im[j]+gi*re[j]);
                double dist=dx*dx+dy*dy;
                if (dist<best) { best=dist; nearest=j; }
            }
            double a=re[nearest],b=im[nearest];
            error+=best; energy+=a*a+b*b;
            cross_r+=x*a+y*b; cross_i+=y*a-x*b;
        }
        signal=(gr*gr+gi*gi)*energy;
        gr=cross_r/energy; gi=cross_i/energy;
    }
    return signal>0 && error>0 ? 10*log10(signal/error) : NAN;
}

// Interpolate the MiniTioune indication in G8GKQ's measured BATC table.
typedef struct { int code, dbm; } RfPoint;
static double interpolate_rf(const RfPoint *points,unsigned count,int code) {
    for (unsigned i=1;i<count;i++) {
        if (code>points[i].code) continue;
        if (code<=points[0].code) return points[0].dbm;
        double fraction=(double)(code-points[i-1].code)/(points[i].code-points[i-1].code);
        return points[i-1].dbm+fraction*(points[i].dbm-points[i-1].dbm);
    }
    return points[count-1].dbm;
}
static double mt_rf_estimate_dbm(unsigned agc1,unsigned agc2) {
    static const RfPoint weak[]={{149,-71},{225,-71},{290,-73},{400,-76},{450,-78},
        {560,-80},{780,-83},{1000,-85},{1480,-88},{2020,-90},{2560,-92},{3200,-94}};
    static const RfPoint strong[]={{10,-71},{21800,-70},{25100,-68},{27100,-66},
        {28900,-62},{31000,-55},{32400,-50},{33600,-44},{35000,-38},{36000,-33},
        {36600,-30},{37600,-26}};
    if (!agc1 && !agc2) return NAN;
    if (agc1<21800) {
        double lower=interpolate_rf(weak,sizeof(weak)/sizeof(*weak),agc2);
        if (agc1<=10) return lower;
        return lower+(-70-lower)*(agc1-10)/21790.0;
    }
    return interpolate_rf(strong,sizeof(strong)/sizeof(*strong),agc1);
}

static int report(MtMonitor *m, MerWindow *window) {
    DecoderStatus s={.magic=DECODER_MONITOR_MAGIC,.type=DECODER_STATUS,.satellite=1,
        .lock_valid=DECODER_OFDM|DECODER_FEC,.mer_db=NAN,.snr_db=NAN,
        .peak_dbfs=NAN,.rms_dbfs=NAN,.cfo_hz=NAN,.pilot_lock=NAN,.pilots=-1,.short_frame=-1,.modcod=-1};
    DecoderMtStatus mt={.bch_flag=-1};
    uint8_t state[11],carrier[3],fec;
    int rc=mt_register_read(m->device,0x69,0xf211,2,state,sizeof(state));
    if (rc) return rc;
    uint8_t gain[2];
    rc=mt_register_read(m->device,0x69,0xf20e,2,gain,2);
    if (rc) return rc;
    mt.agc1=(gain[0]<<8)|gain[1];
    rc=mt_register_read(m->device,0x69,0xf236,2,gain,2);
    if (rc) return rc;
    mt.agc2=(gain[0]<<8)|gain[1];
    mt.rf_input_dbm=mt_rf_estimate_dbm(mt.agc1,mt.agc2);
    mt.frequency_khz=m->tune.frequency_khz;
    mt.lna_gain_db=NAN;
    mt_lna_gain(m->device,m->input_b,&mt.lna_gain_db);
    unsigned mode=(state[10]>>5)&3;
    bool s2=mode==2;
    if (s2) s.satellite=2;
    if ((mode==3 || s2) && (state[1]&8)) {
        s.locks|=DECODER_OFDM;
        rc=mt_register_read(m->device,0x69,s2 ? 0xf369 : 0xf33e,2,&fec,1);
        if (rc) return rc;
        if (fec&(s2 ? 2 : 8)) s.locks|=DECODER_FEC;
        if (s2) {
            if (s.locks & DECODER_FEC) mt.bch_flag=!!(fec & 8);
            unsigned code=(state[0]>>2)&31;
            if (code>0 && code<29) {
                s.modcod=code;
                s.pilots=state[0]&1;
                s.short_frame=(state[0]>>1)&1;
                s.constellation_order=code<=11 ? 4 : code<=17 ? 8 : code<=23 ? 16 : 32;
                snprintf(s.modulation,sizeof(s.modulation),"%s",code<=11 ? "QPSK" : code<=17 ? "8PSK" : code<=23 ? "16APSK" : "32APSK");
                snprintf(s.fec,sizeof(s.fec),"%s",s2_code[code]);
            }
        } else {
            uint8_t puncture;
            rc=mt_register_read(m->device,0x69,0xf33a,2,&puncture,1);
            if (rc) return rc;
            static const char *const rates[32]={[13]="1/2",[18]="2/3",[21]="3/4",[24]="5/6",[25]="6/7",[26]="7/8"};
            const char *rate=rates[puncture&31];
            if (rate) snprintf(s.fec,sizeof(s.fec),"%s",rate);
            snprintf(s.modulation,sizeof(s.modulation),"QPSK");
            s.constellation_order=4;
        }
        rc=mt_register_read(m->device,0x69,0xf24c,2,carrier,3);
        if (rc) return rc;
        int32_t offset=(carrier[0]<<16)|(carrier[1]<<8)|carrier[2];
        if (offset&0x800000) offset-=0x1000000;
        if (m->dx_relock && (s.locks & DECODER_FEC)) {
            // CFRINIT is the signed high 16 bits of the 24-bit CFR estimate.
            m->last_offset=(carrier[0]<<8)|carrier[1];
            m->dx_state=DX_TRACKED;
        }
        s.cfo_hz=offset*(m->master_clock_hz/16777216.0);
    }
    if (m->dx_relock) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC,&now);
        int64_t ms=(int64_t)now.tv_sec*1000+now.tv_nsec/1000000;
        if (s.locks & DECODER_FEC) m->loss_since_ms=0;
        else if (m->dx_state!=DX_IDLE) {
            if (!m->loss_since_ms) m->loss_since_ms=ms;
            int delay=m->dx_state==DX_TRACKED ? 500 : 2000;
            if (ms-m->loss_since_ms>=delay) {
                rc=mt_restart_carrier(m->device,m->dx_state==DX_TRACKED ? m->last_offset : 0);
                if (rc) return rc;
                m->dx_state=m->dx_state==DX_TRACKED ? DX_TRYING : DX_IDLE;
                m->loss_since_ms=ms;
            }
        }
    }
    if (m->socket<0) return 0;
    if (window->order!=s.constellation_order || window->code!=s.modcod) {
        window->used=0;
        window->order=s.constellation_order;
        window->code=s.modcod;
        window->db=NAN;
    }
    DecoderConstellation points={.magic=DECODER_MONITOR_MAGIC,.type=DECODER_CONSTELLATION};
    for (unsigned i=0;i<32 && !atomic_load(&m->finished);++i) {
        uint8_t iq[2];
        rc=mt_register_read(m->device,0x69,0xf202,2,iq,2);
        if (rc) return rc;
        points.iq[2*i]=(int8_t)iq[0]/56.0f;
        points.iq[2*i+1]=(int8_t)iq[1]/56.0f;
        if (s.constellation_order) {
            window->iq[2*window->used]=points.iq[2*i];
            window->iq[2*window->used+1]=points.iq[2*i+1];
            if (++window->used==mer_window) {
                window->db=constellation_mer(window);
                window->used=0;
            }
        }
        ++points.count;
    }
    s.mer_db=window->db;
    mt.rf=s;
    send(m->socket,&mt,sizeof(mt),MSG_DONTWAIT);
    if (points.count) send(m->socket,&points,sizeof(points),MSG_DONTWAIT);
    return 0;
}

void *mt_monitor_run(void *argument) {
    MtMonitor *m=argument;
    MerWindow window={.db=NAN};
    while (!atomic_load(&m->finished)) {
        if (m->live_tuning) {
            uint32_t requests[32];
            ssize_t n=read(STDIN_FILENO,requests,sizeof(requests));
            // Single local writer, fixed-size atomic commands, Hz on the GUI.
            if (n>0 && n%(ssize_t)sizeof(*requests)==0) {
                unsigned khz=requests[n/sizeof(*requests)-1];
                if (khz!=m->tune.frequency_khz) {
                    m->tune.frequency_khz=khz;
                    m->error=mt_tune(m->device,&m->tune);
                    if (m->error) break;
                    m->dx_state=DX_IDLE; m->loss_since_ms=0;
                    window=(MerWindow){.db=NAN};
                    fprintf(stderr,"MT Express tuned to %u kHz\n",khz);
                }
            } else if (!n) m->live_tuning=0;
            else if (n>0 || (errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK)) {
                m->error=LIBUSB_ERROR_IO;
                break;
            }
        }
        m->error=report(m,&window);
        if (m->error) {
            fprintf(stderr,"MT Express: telemetry: %s\n",libusb_error_name(m->error));
            break;
        }
        struct timespec interval={0,50000000};
        nanosleep(&interval,NULL);
    }
    if (m->error && m->live_tuning) {
        fprintf(stderr,"MT Express control: %s\n",libusb_error_name(m->error));
        raise(SIGTERM);
    }
    return NULL;
}
