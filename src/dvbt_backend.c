#define _POSIX_C_SOURCE 200809L
#include "dvbt_backend.h"
#include "decoder_monitor.h"
#include "config.h"
#include "receiver.h"
#include "probe_constellation.h"
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <unistd.h>

static int monitor_fd = -1;

int dvbt_monitor_enabled(void) { return monitor_fd >= 0; }

void dvbt_thread_name(const char *name) { (void)prctl(PR_SET_NAME,name,0,0,0); }

void dvbt_monitor_health(DecoderDvbtHealth *frame) {
    if (monitor_fd < 0) return;
    frame->magic = DECODER_MONITOR_MAGIC;
    frame->type = DECODER_DVBT_HEALTH;
    (void)send(monitor_fd,frame,sizeof(*frame),MSG_DONTWAIT | MSG_NOSIGNAL);
}

void dvbt_monitor_buffer(unsigned queued, unsigned capacity) {
    if (monitor_fd<0) return;
    DecoderBuffer frame={.magic=DECODER_MONITOR_MAGIC,.type=DECODER_BUFFER,
        .queued=queued,.capacity=capacity};
    (void)send(monitor_fd,&frame,sizeof(frame),MSG_DONTWAIT | MSG_NOSIGNAL);
}

void dvbt_monitor_constellation(const float *iq, size_t count) {
    if (monitor_fd < 0) return;
    DecoderConstellation frame = {.magic=DECODER_MONITOR_MAGIC, .type=DECODER_CONSTELLATION};
    frame.count = count > DECODER_POINTS ? DECODER_POINTS : count;
    memcpy(frame.iq, iq, frame.count * 2 * sizeof(float));
    (void)send(monitor_fd, &frame, sizeof(frame), MSG_DONTWAIT | MSG_NOSIGNAL);
}

void dvbt_monitor_status(const rbdvbt_status_context_t *s,
                         const char *service, const char *provider,
                         unsigned locks, unsigned packets, unsigned written,
                         unsigned rs_ok, unsigned rs_bad, unsigned cc_errors) {
    if (monitor_fd < 0 || !s) return;
    DecoderStatus frame = {.magic=DECODER_MONITOR_MAGIC, .type=DECODER_STATUS,
        .locks=locks, .snr_db=s->snr_db, .pilot_lock=s->pilot_lock, .cfo_hz=s->cfo_hz,
        .rms_dbfs=s->iq_rms_dbfs, .peak_dbfs=s->iq_peak_dbfs,
        .buffer_valid=s->fifo1_capacity_symbols > 0,
        .packets=packets, .written_packets=written, .rs_ok=rs_ok,
        .rs_uncorrectable=rs_bad, .cc_errors=cc_errors};
    /* Occupancy of the bounded OFDM -> FEC queue, excluding the active job. */
    if (frame.buffer_valid)
        frame.buffer_percent = fmin(100., 100. * s->fifo1_queued_symbols / s->fifo1_capacity_symbols);
    snprintf(frame.service,sizeof(frame.service),"%s",service ? service : "");
    snprintf(frame.provider,sizeof(frame.provider),"%s",provider ? provider : "");
    snprintf(frame.modulation,sizeof(frame.modulation),"DVB-T 2K QPSK");
    snprintf(frame.fec,sizeof(frame.fec),"%s",s->fec ? s->fec : "—");
    snprintf(frame.guard,sizeof(frame.guard),"%s",s->guard_interval ? s->guard_interval : "—");
    (void)send(monitor_fd, &frame, sizeof(frame), MSG_DONTWAIT | MSG_NOSIGNAL);
}

int dvbt_backend_main(int argc, char **argv) {
    dvbt_thread_name("dvbt-ofdm");
    rbdvbt_config_t cfg;
    if (rbdvbt_parse_args(argc,argv,&cfg)) return 2;
    if (cfg.show_help) { rbdvbt_print_info(argv[0]); return 0; }
    if (cfg.show_version) { puts("linrad_dvb embedded rbdvbt_rx 0.1.5"); return 0; }
    if (cfg.bench_viterbi) return rbdvbt_run_viterbi_benchmark(cfg.bench_viterbi_pairs);
    /* Accept --gui from old configurations, but never open the old X11 windows. */
    cfg.gui = 0;
    const char *port_text = getenv("LINRAD_DECODER_MONITOR_PORT");
    if (port_text) {
        char *end; errno=0;
        long port=strtol(port_text,&end,10);
        if (!errno && end != port_text && !*end && port>0 && port<=65535) {
            struct sockaddr_in addr = {.sin_family=AF_INET,.sin_port=htons(port),
                .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
            monitor_fd=socket(AF_INET,SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,0);
            if (monitor_fd>=0 && connect(monitor_fd,(struct sockaddr *)&addr,sizeof(addr))<0) {
                close(monitor_fd); monitor_fd=-1;
            }
        }
        if (monitor_fd<0) fprintf(stderr,"[DVB-T] decoder telemetry unavailable\n");
    }
    fprintf(stderr,"[DVB-T ingebouwd] threads: dvbt-iq → dvbt-ofdm → dvbt-fec; cores via OS-scheduler\n");
    int result=rbdvbt_run_receiver(&cfg);
    if (monitor_fd>=0) close(monitor_fd);
    monitor_fd=-1;
    return result;
}
