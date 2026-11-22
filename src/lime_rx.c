/* Native LimeSuite RX, following datv-transmitter's Mini device lifecycle.
 * No SoapyLMS7 dependency; this backend never starts a TX stream. */
#include "lime_rx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LIME
#include <lime/LimeSuite.h>
/* Windows resolves the device API when the selected receiver opens. */
#if defined(_WIN32) && !defined(LIME_RX_LINKED_TEST)
#include <windows.h>
#define LIME_DYNAMIC
#define LIME_FUNCTIONS(X) \
    X(Calibrate) X(Close) X(DestroyStream) X(EnableChannel) \
    X(GetDeviceInfo) X(GetDeviceList) X(GetGaindB) X(GetLOFrequency) \
    X(GetLastErrorMessage) X(GetLibraryVersion) X(GetSampleRate) X(GetStreamStatus) \
    X(Init) X(Open) X(RecvStream) X(Reset) \
    X(SetAntenna) X(SetGaindB) X(SetLOFrequency) X(SetLPFBW) \
    X(SetSampleRate) X(SetTestSignal) X(SetupStream) X(StartStream) \
    X(StopStream)
#define LMS_CALL(name, ...) r->api.name(__VA_ARGS__)
#else
#define LMS_CALL(name, ...) LMS_##name(__VA_ARGS__)
#endif

struct LimeRx {
#ifdef LIME_DYNAMIC
    HMODULE library;
    struct {
#define DECLARE(name) __typeof__(&LMS_##name) name;
        LIME_FUNCTIONS(DECLARE)
#undef DECLARE
    } api;
#endif
    lms_device_t *device;
    lms_stream_t stream;
    int setup, started, enabled, path;
    double rate;
    unsigned long long overruns, dropped;
};
#ifdef LIME_DYNAMIC
static int load_limesuite(LimeRx *r) {
    r->library = LoadLibraryExW(L"libLimeSuite.dll", NULL,
        LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!r->library) {
        fprintf(stderr, "[LimeSDR] Cannot load libLimeSuite.dll (Windows error %lu)\n", GetLastError());
        return -1;
    }
#define RESOLVE(name) \
    r->api.name = (__typeof__(r->api.name))(void *)GetProcAddress(r->library, "LMS_" #name); \
    if (!r->api.name) { \
        fprintf(stderr, "[LimeSDR] libLimeSuite.dll is missing LMS_" #name "\n"); \
        return -1; \
    }
    LIME_FUNCTIONS(RESOLVE)
#undef RESOLVE
    return 0;
}
#endif
static int checked(LimeRx *r, int result, const char *operation) {
    (void)r; /* Only the dynamically loaded API needs the receiver here. */
    if (result) fprintf(stderr, "[LimeSDR] %s: %s\n", operation, LMS_CALL(GetLastErrorMessage));
    return result;
}
#define L(call) do { if (checked(r, (call), #call)) return -1; } while (0)
#define OPEN(call) do { if (checked(r, (call), #call)) goto fail; } while (0)

/* Match the complete serial field, never a prefix or a substring. */
static int serial_matches(const char *info, const char *serial) {
    const char *p = info;
    while (*p) {
        while (*p == ' ' || *p == ',') ++p;
        const char *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        const char *trim = end;
        while (trim > p && trim[-1] == ' ') --trim;
        if (trim-p >= 7 && !strncmp(p, "serial=", 7) &&
            (size_t)(trim-p-7) == strlen(serial) && !strncmp(p+7, serial, strlen(serial))) return 1;
        p = end;
    }
    return 0;
}
int lime_rx_gain(LimeRx *r, double *gain) {
    if (!lime_rx_valid(r->rate, LIME_RX_MIN_FREQUENCY, *gain)) return -1;
    L(LMS_CALL(SetGaindB, r->device, LMS_CH_RX, 0, (unsigned)*gain));
    unsigned actual;
    L(LMS_CALL(GetGaindB, r->device, LMS_CH_RX, 0, &actual));
    *gain = actual;
    return 0;
}
int lime_rx_start(LimeRx *r) {
    if (!r->started) {
        L(LMS_CALL(StartStream, &r->stream));
        r->started = 1;
    }
    return 0;
}
int lime_rx_frequency(LimeRx *r, double frequency) {
    if (!lime_rx_valid(r->rate, frequency, 0)) return -1;
    unsigned gain;
    L(LMS_CALL(GetGaindB, r->device, LMS_CH_RX, 0, &gain));
    int restart = r->started;
    if (restart) {
        L(LMS_CALL(StopStream, &r->stream));
        r->started = 0;
    }
    L(LMS_CALL(SetLOFrequency, r->device, LMS_CH_RX, 0, frequency));
    /* The Mini's single RX connector switches between its W and H paths. */
    L(LMS_CALL(SetAntenna, r->device, LMS_CH_RX, 0,
        r->path < 0 ? (frequency < 2e9 ? LMS_PATH_LNAW : LMS_PATH_LNAH) : (size_t)r->path));
    /* Like datv-transmitter, calibrate with a sufficiently wide clock/bandwidth
     * before restoring narrow DATV rates. Analog RX LPF cannot reach 40 kHz. */
    L(LMS_CALL(SetSampleRate, r->device, fmax(r->rate, 4e6), 0));
    L(LMS_CALL(SetLPFBW, r->device, LMS_CH_RX, 0, fmax(r->rate, 1.5e6)));
    L(LMS_CALL(Calibrate, r->device, LMS_CH_RX, 0, fmax(r->rate, 2.5e6), 0));
    L(LMS_CALL(SetSampleRate, r->device, r->rate, 0));
    L(LMS_CALL(SetGaindB, r->device, LMS_CH_RX, 0, gain));
    double actual;
    L(LMS_CALL(GetSampleRate, r->device, LMS_CH_RX, 0, &actual, NULL));
    if (!isfinite(actual) || fabs(actual-r->rate) > r->rate*1e-6) {
        fprintf(stderr, "[LimeSDR] Requested rate %.0f, hardware rate %.3f; refusing mismatched IQ\n", r->rate, actual);
        return -1;
    }
    L(LMS_CALL(GetLOFrequency, r->device, LMS_CH_RX, 0, &actual));
    if (!isfinite(actual) || fabs(actual-frequency) > fmax(1, frequency*1e-6)) {
        fprintf(stderr, "[LimeSDR] Requested frequency %.0f, hardware frequency %.3f\n", frequency, actual);
        return -1;
    }
    return restart ? lime_rx_start(r) : 0;
}
LimeRx *lime_rx_open(const char *args, const char *antenna, double rate, double frequency, double *gain, double *actual) {
    const char *serial = "";
    if (*args) {
        if (strncmp(args, "serial=", 7) || !args[7] || strpbrk(args+7, ", \t\r\n")) {
            fprintf(stderr, "[LimeSDR] Device arguments: empty or serial=SERIAL\n"); return NULL;
        }
        serial = args+7;
    }
    int path = -1;
    if (*antenna && strcmp(antenna, "AUTO")) {
        if (!strcmp(antenna, "LNAW")) path = LMS_PATH_LNAW;
        else if (!strcmp(antenna, "LNAH")) path = LMS_PATH_LNAH;
        else { fprintf(stderr, "[LimeSDR] Antenna: AUTO, LNAW or LNAH\n"); return NULL; }
    }
    if (!lime_rx_valid(rate, frequency, *gain)) return NULL;
    LimeRx *r = calloc(1, sizeof(*r));
    lms_info_str_t *list = NULL;
    if (!r) return NULL;
#ifdef LIME_DYNAMIC
    if (load_limesuite(r)) goto fail;
#endif
    r->rate = rate; r->path = path;
    int count = LMS_CALL(GetDeviceList, NULL);
    if (count <= 0 || count > 1024) {
        fprintf(stderr, "[LimeSDR] No devices found, enumeration failed or too many devices\n"); goto fail;
    }
    /* API has no capacity parameter; allow headroom for ordinary hotplug. */
    list = calloc((size_t)count+1024, sizeof(*list));
    if (!list) goto fail;
    count = LMS_CALL(GetDeviceList, list);
    int selected = -1;
    for (int i = 0; i < count; ++i) {
        if (*serial && !serial_matches(list[i], serial)) continue;
        if (selected >= 0) {
            fprintf(stderr, "[LimeSDR] Multiple devices; select one with --args serial=SERIAL\n"); goto fail;
        }
        selected = i;
    }
    if (selected < 0) { fprintf(stderr, "[LimeSDR] Selected device not found\n"); goto fail; }
    OPEN(LMS_CALL(Open, &r->device, list[selected], NULL));
    free(list); list = NULL;
    const lms_dev_info_t *info = LMS_CALL(GetDeviceInfo, r->device);
    if (!info || strcmp(info->deviceName, "LimeSDR-Mini")) {
        fprintf(stderr, "[LimeSDR] This backend supports the original LimeSDR Mini v1.x (including v1.2)\n"); goto fail;
    }
    fprintf(stderr, "[LimeSDR] %s | hardware %s | firmware %s | gateware %s | LimeSuite %s\n",
        info->deviceName, info->hardwareVersion, info->firmwareVersion, info->gatewareVersion, LMS_CALL(GetLibraryVersion));
    OPEN(LMS_CALL(Reset, r->device));
    OPEN(LMS_CALL(Init, r->device));
    OPEN(LMS_CALL(EnableChannel, r->device, LMS_CH_TX, 0, false));
    OPEN(LMS_CALL(EnableChannel, r->device, LMS_CH_RX, 0, true));
    r->enabled = 1;
    if (lime_rx_gain(r, gain) || lime_rx_frequency(r, frequency)) goto fail;
    /* Calibration may temporarily adjust gain; restore the requested setting. */
    if (lime_rx_gain(r, gain)) goto fail;
    OPEN(LMS_CALL(SetTestSignal, r->device, LMS_CH_RX, 0, LMS_TESTSIG_NONE, 0, 0));
    OPEN(LMS_CALL(GetSampleRate, r->device, LMS_CH_RX, 0, actual, NULL));
    r->stream = (lms_stream_t){.channel=0, .fifoSize=1024*1024, .throughputVsLatency=.5,
        .isTx=false, .dataFmt=LMS_FMT_I16, .linkFmt=LMS_LINK_FMT_I16};
    OPEN(LMS_CALL(SetupStream, r->device, &r->stream));
    r->setup = 1;
    return r;
fail:
    free(list);
    lime_rx_close(r);
    return NULL;
}
int lime_rx_read(LimeRx *r, int16_t *iq, size_t samples) {
    int n = LMS_CALL(RecvStream, &r->stream, iq, samples, NULL, 100);
    if (n < 0) checked(r, n, "LMS_RecvStream");
    return n;
}
int lime_rx_report(LimeRx *r) {
    lms_stream_status_t status;
    L(LMS_CALL(GetStreamStatus, &r->stream, &status));
    r->overruns += status.overrun; r->dropped += status.droppedPackets;
    if (status.overrun || status.droppedPackets)
        fprintf(stderr, "[LimeSDR] RX overruns %llu, dropped packets %llu total; samples lost, continuing RX\n", r->overruns, r->dropped);
    if (!status.active) { fprintf(stderr, "[LimeSDR] RX stream became inactive\n"); return -1; }
    return 0;
}
void lime_rx_close(LimeRx *r) {
    if (!r) return;
    if (r->started) checked(r, LMS_CALL(StopStream, &r->stream), "LMS_StopStream");
    if (r->setup) checked(r, LMS_CALL(DestroyStream, r->device, &r->stream), "LMS_DestroyStream");
    if (r->enabled) checked(r, LMS_CALL(EnableChannel, r->device, LMS_CH_RX, 0, false), "RX disable");
    if (r->device) checked(r, LMS_CALL(Close, r->device), "LMS_Close");
#ifdef LIME_DYNAMIC
    if (r->library) FreeLibrary(r->library);
#endif
    free(r);
}
#else
LimeRx *lime_rx_open(const char *args, const char *antenna, double rate, double frequency, double *gain, double *actual) {
    (void)args; (void)antenna; (void)rate; (void)frequency; (void)gain; (void)actual;
    fprintf(stderr, "[LimeSDR] Built without LimeSuite; install LimeSuite development files and rebuild (see README)\n");
    return NULL;
}
int lime_rx_start(LimeRx *r) { (void)r; return -1; }
int lime_rx_frequency(LimeRx *r, double f) { (void)r; (void)f; return -1; }
int lime_rx_gain(LimeRx *r, double *g) { (void)r; (void)g; return -1; }
int lime_rx_read(LimeRx *r, int16_t *iq, size_t n) { (void)r; (void)iq; (void)n; return -1; }
int lime_rx_report(LimeRx *r) { (void)r; return -1; }
void lime_rx_close(LimeRx *r) { (void)r; }
#endif
