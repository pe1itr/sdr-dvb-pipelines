/* UHD calls below are mocks: this test cannot open a device or emit RF. */
#define _POSIX_C_SOURCE 200809L
#include "../src/b210_mixer.h"
#include <assert.h>
#include <time.h>
static double tx_rate, tx_bandwidth, tx_frequency, tx_gain;
static size_t tx_channel;
static int coerce;
static atomic_int sent_blocks, end_bursts;
#define DEVICE(h,c) do {(void)(h);tx_channel=(c);} while (0)
uhd_error uhd_usrp_set_tx_rate(uhd_usrp_handle h,double v,size_t c) {DEVICE(h,c);tx_rate=v;return UHD_ERROR_NONE;}
uhd_error uhd_usrp_get_tx_rate(uhd_usrp_handle h,size_t c,double *v) {DEVICE(h,c);*v=tx_rate+coerce;return UHD_ERROR_NONE;}
uhd_error uhd_usrp_set_tx_bandwidth(uhd_usrp_handle h,double v,size_t c) {DEVICE(h,c);tx_bandwidth=v;return UHD_ERROR_NONE;}
uhd_error uhd_usrp_get_tx_bandwidth(uhd_usrp_handle h,size_t c,double *v) {DEVICE(h,c);*v=tx_bandwidth;return UHD_ERROR_NONE;}
uhd_error uhd_usrp_set_tx_gain(uhd_usrp_handle h,double v,size_t c,const char *name) {DEVICE(h,c);(void)name;tx_gain=v;return UHD_ERROR_NONE;}
uhd_error uhd_usrp_get_tx_gain(uhd_usrp_handle h,size_t c,const char *name,double *v) {DEVICE(h,c);(void)name;*v=tx_gain;return UHD_ERROR_NONE;}
uhd_error uhd_usrp_set_tx_antenna(uhd_usrp_handle h,const char *v,size_t c) {DEVICE(h,c);assert(!strcmp(v,"TX/RX"));return UHD_ERROR_NONE;}
uhd_error uhd_usrp_set_tx_freq(uhd_usrp_handle h,uhd_tune_request_t *v,size_t c,uhd_tune_result_t *out) {
    DEVICE(h,c);(void)out;tx_frequency=v->target_freq;return UHD_ERROR_NONE;
}
uhd_error uhd_usrp_get_tx_freq(uhd_usrp_handle h,size_t c,double *v) {DEVICE(h,c);*v=tx_frequency;return UHD_ERROR_NONE;}
uhd_error uhd_tx_streamer_make(uhd_tx_streamer_handle *h) {*h=(uhd_tx_streamer_handle)1;return UHD_ERROR_NONE;}
uhd_error uhd_tx_streamer_free(uhd_tx_streamer_handle *h) {*h=NULL;return UHD_ERROR_NONE;}
uhd_error uhd_usrp_get_tx_stream(uhd_usrp_handle h,uhd_stream_args_t *a,uhd_tx_streamer_handle stream) {
    (void)h;(void)stream;assert(a->n_channels==1 && a->channel_list[0]==tx_channel);
    assert(!strcmp(a->cpu_format,"fc32"));return UHD_ERROR_NONE;
}
uhd_error uhd_tx_streamer_send(uhd_tx_streamer_handle h,const void **buffers,size_t count,
        uhd_tx_metadata_handle *metadata,double timeout,size_t *sent) {
    (void)h;(void)timeout;*sent=count;
    bool end=false;assert(!uhd_tx_metadata_end_of_burst(*metadata,&end));
    if (!count) {assert(end);atomic_fetch_add(&end_bursts,1);return UHD_ERROR_NONE;}
    const float *iq=buffers[0];
    double step=2*3.14159265358979323846*2500000/8192000;
    for (size_t i=0;i<count;++i) {
        assert(fabs(hypot(iq[2*i],iq[2*i+1])-.05)<1e-7);
        if (i) {
            double dot=(iq[2*i]*iq[2*i-2]+iq[2*i+1]*iq[2*i-1])/.0025;
            assert(fabs(dot-cos(step))<1e-6);
        }
    }
    atomic_fetch_add(&sent_blocks,1);return UHD_ERROR_NONE;
}
int main(void) {
    size_t c;const char *a;
    assert(!b210_input("RX1",&c,&a) && c==0 && !strcmp(a,"RX2"));
    assert(!b210_input("RX2",&c,&a) && c==1 && !strcmp(a,"RX2"));
    assert(!b210_input("TRX2",&c,&a) && c==1 && !strcmp(a,"TX/RX"));
    assert(b210_input("RX3",&c,&a));
    assert(b210_mixer_valid(229072000,53000000,"RX1","TRX1",1));
    assert(!b210_mixer_valid(229072000,53000000,"TRX1","TRX1",1));
    assert(b210_mixer_valid(145500000,53000000,"RX1","TRX1",1));
    assert(b210_mixer_valid(5700000000.0,53000000,"RX1","TRX1",1));
    assert(!b210_mixer_valid(70000000,53000000,"RX1","TRX1",1));
    assert(!b210_mixer_valid(229072000,NAN,"RX1","TRX1",1));
    B210Mixer mixer={0};
    assert(!b210_mixer_open(&mixer,NULL,"TRX2",229072000,53000000));
    assert(tx_channel==1 && tx_rate==8192000 && tx_bandwidth==6000000 && tx_gain==0);
    assert(tx_frequency==173572000); // synthesizer + 2.5 MHz tone = 176.072 MHz LO
    for (int i=0;i<100 && !atomic_load(&sent_blocks);++i) {struct timespec t={0,1000000};nanosleep(&t,NULL);}
    b210_mixer_close(&mixer);
    assert(atomic_load(&sent_blocks)>0 && atomic_load(&end_bursts)==1);
    coerce=100;assert(b210_mixer_open(&mixer,NULL,"TRX1",229072000,53000000));
    assert(!mixer.started);b210_mixer_close(&mixer);
    puts("B210 mixer mocks: PASS");return 0;
}
