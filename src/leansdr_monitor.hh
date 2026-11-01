/* Included after runtime_common in the embedded LeanDVB translation unit. */
#include "decoder_monitor.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

static DecoderStatus satellite_status;
static double satellite_fec_time;
static double monitor_seconds() {
    timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9;
}
extern "C" void linrad_fec_result(int good, int corrected) {
    (void)corrected;
    if (good) ++satellite_status.fec_good; else ++satellite_status.fec_bad;
    satellite_status.lock_valid |= DECODER_FEC;
    if (good) satellite_status.locks |= DECODER_FEC;
    else satellite_status.locks &= ~DECODER_FEC;
    satellite_fec_time=monitor_seconds();
}
static void satellite_modulation(int mc) {
    auto info=&modcod_infos[mc];
    satellite_status.constellation_order=info->nsymbols;
    snprintf(satellite_status.modulation,sizeof(satellite_status.modulation),"%s (MC %d)",cstln_base::names[info->c],mc);
    auto fec=&fec_specs[info->rate];
    snprintf(satellite_status.fec,sizeof(satellite_status.fec),"%d/%d",fec->bits_in,fec->bits_out);
}
extern "C" void linrad_s2_pls(int mc, int sf, int pilots) {
    if (mc<1 || mc>28 || !modcod_infos[mc].nslots_nf) return;
    satellite_status.modcod=mc; satellite_status.short_frame=sf; satellite_status.pilots=pilots;
    satellite_modulation(mc);
}

struct linrad_monitor : runnable {
    pipereader<float> freq, mer;
    pipereader<int> lock, frame;
    pipereader<cf32> points;
    pipereader<tspacket> packets;
    float rate;
    int fd=-1;
    double sent=0, measured=0, received=0;
    linrad_monitor(runtime_common &r, const config &cfg)
      : runnable(r.sch,"linrad monitor"), freq(*r.p_freq), mer(*r.p_mer),
        lock(*r.p_lock), frame(*r.p_framelock), points(*r.p_cstln), packets(*r.p_tspackets), rate(cfg.Fs) {
        memset(&satellite_status,0,sizeof(satellite_status));
        satellite_status.magic=DECODER_MONITOR_MAGIC; satellite_status.type=DECODER_STATUS;
        satellite_status.satellite=cfg.standard==config::DVB_S2 ? 2 : 1;
        satellite_status.modcod=satellite_status.short_frame=satellite_status.pilots=-1;
        satellite_status.snr_db=satellite_status.mer_db=satellite_status.cfo_hz=NAN;
        satellite_status.pilot_lock=satellite_status.rms_dbfs=satellite_status.peak_dbfs=NAN;
        satellite_status.constellation_order=4;
        if (cfg.standard==config::DVB_S) {
            snprintf(satellite_status.modulation,sizeof(satellite_status.modulation),"DVB-S QPSK");
            auto fec=&fec_specs[cfg.fec];
            snprintf(satellite_status.fec,sizeof(satellite_status.fec),"%d/%d",fec->bits_in,fec->bits_out);
        } else if (cfg.modcods && !(cfg.modcods & (cfg.modcods-1))) {
            // The GUI chooses one MODCOD. Show that fixed choice even before
            // lock, keeping frame length and pilots unknown until confirmed.
            for (int mc=1; mc<=28; ++mc)
                if ((cfg.modcods & (uint32_t(1)<<mc)) && modcod_infos[mc].nslots_nf)
                    satellite_modulation(mc);
        }
        const char *port=getenv("LINRAD_DECODER_MONITOR_PORT");
        char *end=nullptr; long number=port ? strtol(port,&end,10) : 0;
        if (port && end!=port && !*end && number>0 && number<=65535) {
            sockaddr_in addr={}; addr.sin_family=AF_INET; addr.sin_port=htons(number);
            addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
            if (fd>=0 && connect(fd,reinterpret_cast<sockaddr *>(&addr),sizeof(addr))<0) { close(fd); fd=-1; }
        }
    }
    template<typename T> bool latest(pipereader<T> &p,T &value) {
        size_t n=p.readable(); if (!n) return false;
        value=p.rd()[n-1]; p.read(n); return true;
    }
    void run() {
        double now=monitor_seconds();
        float value;
        if (latest(freq,value)) { satellite_status.cfo_hz=value*rate; measured=now; }
        if (latest(mer,value)) { satellite_status.mer_db=value; measured=now; }
        int state;
        if (latest(frame,state)) {
            satellite_status.lock_valid |= DECODER_OFDM;
            if (state) satellite_status.locks |= DECODER_OFDM;
            else satellite_status.locks &= ~DECODER_OFDM;
        }
        if (latest(lock,state) && satellite_status.satellite==1) {
            satellite_status.lock_valid |= DECODER_OFDM;
            if (state) satellite_status.locks |= DECODER_OFDM;
            else satellite_status.locks &= ~DECODER_OFDM;
        }
        size_t n=packets.readable();
        if (n) {
            satellite_status.packets+=n; satellite_status.written_packets+=n;
            packets.read(n); received=now;
        }
        satellite_status.lock_valid |= DECODER_TS;
        if (received && now-received<3) satellite_status.locks |= DECODER_TS;
        else satellite_status.locks &= ~DECODER_TS;
        if (now-satellite_fec_time>10) satellite_status.lock_valid &= ~DECODER_FEC;
        if (measured && now-measured>10) {
            satellite_status.mer_db=satellite_status.cfo_hz=NAN;
            satellite_status.lock_valid &= ~DECODER_OFDM;
        }
        n=points.readable();
        bool publish=fd>=0 && now-sent>=.05;
        if (publish) {
            send(fd,&satellite_status,sizeof(satellite_status),MSG_DONTWAIT|MSG_NOSIGNAL);
            if (n) {
                DecoderConstellation out={}; out.magic=DECODER_MONITOR_MAGIC;
                out.type=DECODER_CONSTELLATION; out.count=std::min(n,size_t(DECODER_POINTS));
                for (unsigned i=0;i<out.count;++i) {
                    const auto &p=points.rd()[i*n/out.count];
                    out.iq[2*i]=p.re/cstln_amp; out.iq[2*i+1]=p.im/cstln_amp;
                }
                send(fd,&out,sizeof(out),MSG_DONTWAIT|MSG_NOSIGNAL);
            }
            sent=now;
        }
        points.read(n);
    }
    void shutdown() { if (fd>=0) close(fd); fd=-1; }
};
