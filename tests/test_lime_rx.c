/* Native backend tests against the actual LimeSuite headers, without USB. */
#include <lime/LimeSuite.h>
#include "../src/lime_rx.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int calls, fail_at, opened, enabled, setup, started, count=1;
static int bad_rate, bad_frequency, read_result=3, inactive, calibrations;
static unsigned gain, path, overruns, dropped;
static double rate, frequency;
static const char *name="LimeSDR-Mini";
static int step(void) { return ++calls == fail_at ? -1 : 0; }
static void channel(lms_device_t *d, bool tx, size_t ch) { assert(d && opened && !tx && ch==0); }
int LMS_GetDeviceList(lms_info_str_t *list) {
    if (step()) return -1;
    if (list) for (int i=0; i<count; ++i)
        snprintf(list[i], sizeof(list[i]), "LimeSDR Mini, media=USB 3.0, serial=%d", 123+i);
    return count;
}
int LMS_Open(lms_device_t **d, const lms_info_str_t info, void *args) {
    (void)args; assert(!opened && strstr(info,"serial="));
    if (step()) return -1;
    *d=(void *)&opened; opened=1; return 0;
}
int LMS_Close(lms_device_t *d) { assert(d && opened && !setup && !started); opened=enabled=0; return 0; }
const lms_dev_info_t *LMS_GetDeviceInfo(lms_device_t *d) {
    static lms_dev_info_t info;
    assert(d && opened); snprintf(info.deviceName,sizeof(info.deviceName),"%s",name);
    return step() ? NULL : &info;
}
const char *LMS_GetLibraryVersion(void) { return "mock"; }
const char *LMS_GetLastErrorMessage(void) { return "injected failure"; }
int LMS_Reset(lms_device_t *d) { assert(d && opened); return step(); }
int LMS_Init(lms_device_t *d) { assert(d && opened); return step(); }
int LMS_EnableChannel(lms_device_t *d, bool tx, size_t ch, bool enable) {
    assert(d && opened && ch==0);
    if (tx) assert(!enable); /* Receiving must never enable TX. */
    if (step()) return -1;
    if (!tx) enabled=enable;
    return 0;
}
int LMS_SetGaindB(lms_device_t *d, bool tx, size_t ch, unsigned value) {
    channel(d,tx,ch); if (step()) return -1; gain=value; return 0;
}
int LMS_GetGaindB(lms_device_t *d, bool tx, size_t ch, unsigned *value) {
    channel(d,tx,ch); if (step()) return -1; *value=gain; return 0;
}
int LMS_SetLOFrequency(lms_device_t *d, bool tx, size_t ch, double value) {
    channel(d,tx,ch); assert(!started); if (step()) return -1; frequency=value; return 0;
}
int LMS_GetLOFrequency(lms_device_t *d, bool tx, size_t ch, double *value) {
    channel(d,tx,ch); if (step()) return -1; *value=frequency+bad_frequency*10000; return 0;
}
int LMS_SetAntenna(lms_device_t *d, bool tx, size_t ch, size_t value) {
    channel(d,tx,ch); assert(value==LMS_PATH_LNAW || value==LMS_PATH_LNAH);
    if (step()) return -1;
    path=value; return 0;
}
int LMS_SetSampleRate(lms_device_t *d, double value, size_t oversample) {
    assert(d && opened && !started && !oversample); if (step()) return -1; rate=value; return 0;
}
int LMS_GetSampleRate(lms_device_t *d, bool tx, size_t ch, double *host, double *rf) {
    channel(d,tx,ch); (void)rf; if (step()) return -1; *host=rate+bad_rate*100; return 0;
}
int LMS_SetLPFBW(lms_device_t *d, bool tx, size_t ch, double bw) {
    channel(d,tx,ch); assert(bw>=1.5e6 && !started); return step();
}
int LMS_Calibrate(lms_device_t *d, bool tx, size_t ch, double bw, unsigned flags) {
    channel(d,tx,ch); assert(rate>=4e6 && bw>=2.5e6 && flags==0 && !started);
    if (step()) return -1;
    ++calibrations; gain=55; return 0; /* Backend must restore gain. */
}
int LMS_SetTestSignal(lms_device_t *d, bool tx, size_t ch, lms_testsig_t sig, int16_t i, int16_t q) {
    channel(d,tx,ch); assert(sig==LMS_TESTSIG_NONE && !i && !q); return step();
}
int LMS_SetupStream(lms_device_t *d, lms_stream_t *s) {
    assert(d && opened && enabled && !setup && !s->isTx && s->channel==0);
    assert(s->dataFmt==LMS_FMT_I16 && s->linkFmt==LMS_LINK_FMT_I16);
    if (step()) return -1;
    setup=1; return 0;
}
int LMS_StartStream(lms_stream_t *s) {
    (void)s; assert(setup && !started); if (step()) return -1; started=1; return 0;
}
int LMS_StopStream(lms_stream_t *s) {
    (void)s; assert(setup && started); if (step()) return -1; started=0; return 0;
}
int LMS_DestroyStream(lms_device_t *d, lms_stream_t *s) {
    (void)d; (void)s; assert(setup && !started); setup=0; return 0;
}
int LMS_RecvStream(lms_stream_t *s, void *iq, size_t n, lms_stream_meta_t *meta, unsigned timeout) {
    (void)s; (void)meta; assert(started && n>=3 && timeout<=100);
    static const int16_t samples[]={-32768,32752,0,-16,1600,-3200};
    if (read_result>0) memcpy(iq,samples,sizeof(samples));
    return read_result;
}
int LMS_GetStreamStatus(lms_stream_t *s, lms_stream_status_t *status) {
    (void)s; assert(started); if (step()) return -1;
    *status=(lms_stream_status_t){.active=!inactive,.overrun=overruns,.droppedPackets=dropped};
    overruns=dropped=0; return 0;
}
static LimeRx *open_rx(const char *args, const char *antenna, double sample_rate) {
    double g=30, actual=0;
    LimeRx *r=lime_rx_open(args,antenna,sample_rate,437e6,&g,&actual);
    if (r) { assert(actual==sample_rate && gain==30 && g==30); }
    return r;
}
static void closed(void) { assert(!opened && !enabled && !setup && !started); }
int main(void) {
    LimeRx *r=open_rx("","AUTO",100000); assert(r && path==LMS_PATH_LNAW);
    int open_calls=calls;
    assert(!lime_rx_start(r));
    int16_t iq[8]={0}; assert(lime_rx_read(r,iq,4)==3);
    assert(iq[0]==-32768 && iq[1]==32752 && iq[5]==-3200);
    read_result=0; assert(!lime_rx_read(r,iq,4));
    read_result=-1; assert(lime_rx_read(r,iq,4)==-1);
    assert(!lime_rx_frequency(r,2400e6) && path==LMS_PATH_LNAH && started && gain==30 && rate==100000);
    double g=73; assert(!lime_rx_gain(r,&g) && g==73);
    assert(!lime_rx_frequency(r,10e6) && path==LMS_PATH_LNAW && gain==73);
    overruns=2; dropped=3; assert(!lime_rx_report(r)); assert(!overruns && !dropped);
    inactive=1; assert(lime_rx_report(r)==-1); inactive=0;
    lime_rx_close(r); closed();
    /* Every failing initialization operation releases partially acquired resources. */
    for (int failure=1; failure<=open_calls; ++failure) {
        calls=0; fail_at=failure; assert(!open_rx("","AUTO",1e6)); closed();
    }
    fail_at=0; count=0; assert(!open_rx("","AUTO",1e6)); closed();
    count=2; assert(!open_rx("","AUTO",1e6)); closed();
    assert(!open_rx("serial=12","AUTO",1e6)); closed();
    r=open_rx("serial=124","LNAW",30720000); assert(r);
    assert(!lime_rx_start(r)); assert(!lime_rx_frequency(r,3.5e9) && path==LMS_PATH_LNAW);
    lime_rx_close(r); closed(); count=1;
    name="LimeSDR-USB"; assert(!open_rx("","AUTO",1e6)); closed(); name="LimeSDR-Mini";
    assert(!open_rx("driver=lime","AUTO",1e6)); assert(!open_rx("serial=","AUTO",1e6));
    assert(!open_rx("","LNAL",1e6)); closed();
    bad_rate=1; assert(!open_rx("","AUTO",1e6)); closed(); bad_rate=0;
    bad_frequency=1; assert(!open_rx("","AUTO",1e6)); closed(); bad_frequency=0;
    r=open_rx("","AUTO",1e6); assert(r); fail_at=calls+1;
    assert(lime_rx_start(r)==-1); lime_rx_close(r); closed(); fail_at=0;
    assert(calibrations>0);
    puts("Lime RX: lifecycle, selection, calibration, tuning, samples and failures passed");
    return 0;
}

#ifdef _WIN32
/* Link the backend to this test's LMS functions, independent of installed hardware. */
#define HAVE_LIME
#define LIME_RX_LINKED_TEST
#include "../src/lime_rx.c"
#endif
