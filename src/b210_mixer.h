#ifndef B210_MIXER_H
#define B210_MIXER_H
// Conducted low-side mixer LO, adapted from dab-monitor's measured B210 setup.
// RX rate must be established before configuring TX (shared B2x0 clock plan).
#include <uhd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uhd_tx_streamer_handle stream;
    pthread_t thread;
    atomic_int running, failed;
    int started;
    size_t channel;
} B210Mixer;
static int b210_input(const char *input,size_t *channel,const char **antenna) {
    if (!strcmp(input,"TRX1")) {*channel=0;*antenna="TX/RX";}
    else if (!strcmp(input,"RX1")) {*channel=0;*antenna="RX2";}
    else if (!strcmp(input,"TRX2")) {*channel=1;*antenna="TX/RX";}
    else if (!strcmp(input,"RX2")) {*channel=1;*antenna="RX2";}
    else return -1;
    return 0;
}
static int b210_mixer_valid(double rf,double intermediate,const char *rx,const char *tx,int enabled) {
    size_t channel;const char *antenna;
    return isfinite(rf) && isfinite(intermediate) && intermediate>=46000000 && intermediate<=126428000 &&
        rf>=174928000 && rf<=239200000 && rf-intermediate-2500000>=46000000 &&
        !b210_input(rx,&channel,&antenna) && (!strcmp(tx,"TRX1") || !strcmp(tx,"TRX2")) &&
        (!enabled || strcmp(rx,tx));
}
static void *b210_lo_worker(void *data) {
    B210Mixer *m=data;
    // 8 ms of headroom, continuous phase; no DC carrier/leakage over the wanted IF.
    const size_t count=65536;float *iq=malloc(count*2*sizeof(float));
    uhd_tx_metadata_handle first=NULL, body=NULL, end=NULL;
    if (!iq || uhd_tx_metadata_make(&first,false,0,0,true,false) ||
        uhd_tx_metadata_make(&body,false,0,0,false,false) ||
        uhd_tx_metadata_make(&end,false,0,0,false,true)) {atomic_store(&m->failed,1);goto done;}
    double phase=0, step=2*3.14159265358979323846*2500000/8192000;
    int initial=1;
    while (atomic_load(&m->running)) {
        for (size_t i=0;i<count;++i) {
            iq[2*i]=0.05*cos(phase);iq[2*i+1]=0.05*sin(phase);
            phase+=step;if (phase>=2*3.14159265358979323846) phase-=2*3.14159265358979323846;
        }
        const void *buffers[]={iq};size_t sent=0;
        uhd_tx_metadata_handle *md=initial ? &first : &body;
        if (uhd_tx_streamer_send(m->stream,buffers,count,md,.5,&sent) || sent!=count) {
            fprintf(stderr,"B210 mixer LO: TX failed/partial send (%zu/%zu); stopping receiver\n",sent,count);
            atomic_store(&m->failed,1);break;
        }
        initial=0;
    }
    {const void *buffers[]={iq};size_t sent=0;uhd_tx_streamer_send(m->stream,buffers,0,&end,.2,&sent);}
done:
    if (first) uhd_tx_metadata_free(&first);
    if (body) uhd_tx_metadata_free(&body);
    if (end) uhd_tx_metadata_free(&end);
    free(iq);return NULL;
}
static void b210_mixer_stop(B210Mixer *m) {
    atomic_store(&m->running,0);
    if (m->started) {pthread_join(m->thread,NULL);m->started=0;}
}
static int b210_mixer_tune(B210Mixer *m,uhd_usrp_handle usrp,double rf,double intermediate) {
    b210_mixer_stop(m);
    double synth=rf-intermediate-2500000, actual=0;
    if (!isfinite(synth) || synth<46000000) return -1;
    uhd_tune_request_t tune={.target_freq=synth,.rf_freq_policy=UHD_TUNE_REQUEST_POLICY_AUTO,
        .dsp_freq_policy=UHD_TUNE_REQUEST_POLICY_AUTO,.args=""};
    uhd_tune_result_t result;
    if (uhd_usrp_set_tx_freq(usrp,&tune,m->channel,&result) ||
        uhd_usrp_get_tx_freq(usrp,m->channel,&actual) || fabs(actual-synth)>2) return -1;
    atomic_store(&m->failed,0);atomic_store(&m->running,1);
    if (pthread_create(&m->thread,NULL,b210_lo_worker,m)) {atomic_store(&m->running,0);return -1;}
    m->started=1;
    fprintf(stderr,"B210 conducted mixer LO: TRX%zu RF=%.0f IF=%.0f LO=%.0f synth=%.0f; gain=0 IQ=0.05\n",
        m->channel+1,rf,intermediate,rf-intermediate,synth);
    return 0;
}
static int b210_mixer_open(B210Mixer *m,uhd_usrp_handle usrp,const char *port,double rf,double intermediate) {
    m->channel=!strcmp(port,"TRX2") ? 1 : 0;
    double actual=0;
    if (uhd_usrp_set_tx_rate(usrp,8192000,m->channel) ||
        uhd_usrp_get_tx_rate(usrp,m->channel,&actual) || fabs(actual-8192000)>2 ||
        uhd_usrp_set_tx_bandwidth(usrp,6000000,m->channel) ||
        uhd_usrp_get_tx_bandwidth(usrp,m->channel,&actual) || fabs(actual-6000000)>2 ||
        uhd_usrp_set_tx_antenna(usrp,"TX/RX",m->channel) ||
        uhd_usrp_set_tx_gain(usrp,0,m->channel,"") ||
        uhd_usrp_get_tx_gain(usrp,m->channel,"",&actual) || fabs(actual)>.51 ||
        uhd_tx_streamer_make(&m->stream)) return -1;
    uhd_stream_args_t args={.cpu_format="fc32",.otw_format="sc16",.args="",.channel_list=&m->channel,.n_channels=1};
    if (uhd_usrp_get_tx_stream(usrp,&args,m->stream)) return -1;
    return b210_mixer_tune(m,usrp,rf,intermediate);
}
static void b210_mixer_close(B210Mixer *m) {
    b210_mixer_stop(m);
    if (m->stream) uhd_tx_streamer_free(&m->stream);
}
#endif
