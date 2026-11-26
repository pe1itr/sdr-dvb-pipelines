#define _POSIX_C_SOURCE 200809L
#include "../components/moon/moon.h"
#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Version.h>
#include <uhd.h>
#include "b210_mixer.h"
#include "../components/dab/channels.h"
#include "iq_output.h"
#include "iq_convert.h"
#include "hackrf_gain.h"
#include "iq_shift.h"
#include "rx_limits.h"
#include "sdriq.h"
#include "funcube.h"
#include "rtlsdr.h"
#include "lime_rx.h"
#include "pluto_rx.h"
#include "snapshot_file.h"
#include "afedri.h"
#include "tune_control.h"
#include "iq_bandfilter.h"
#include "iq_monitor.h"
#include "iq_record.h"
#include "sdr_rx_status.h"
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#if SOAPY_SDR_API_VERSION < 0x00080000
/* SoapySDR 0.7 returns malloc-owned arrays/strings on Linux. */
#define SoapySDR_free free
#endif
#include <string.h>
#include <unistd.h>
#include <time.h>
#ifdef _WIN32
#include <glib/gstdio.h>
#endif

static volatile sig_atomic_t stopped;
static void stop(int sig) { (void)sig; stopped = 1; }
static double monotonic_seconds(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec/1e9;
}
static void report_output(IQOutput *output, double rate, int sample_bytes,
                          double elapsed, IQOutputStats *previous, const char *label) {
    IQOutputStats stats; iq_output_stats(output, &stats);
    fprintf(stderr, "[SDR] %s: RX %.2f MS/s, decoder input %.2f MS/s; queued %.1f/%.1f ms (peak %.1f ms); dropped %llu samples total\n",
            label, (stats.accepted+stats.dropped-previous->accepted-previous->dropped)/elapsed/sample_bytes/1e6,
            (stats.written-previous->written)/elapsed/sample_bytes/1e6,
            stats.queued/(rate*sample_bytes)*1000, stats.capacity/(rate*sample_bytes)*1000,
            stats.peak/(rate*sample_bytes)*1000, (unsigned long long)(stats.dropped/sample_bytes));
    if (stats.write_stalls!=previous->write_stalls)
        fprintf(stderr,"[SDR] IQ writer: %llu backpressure waits this interval (%llu total)\n",
            (unsigned long long)(stats.write_stalls-previous->write_stalls),
            (unsigned long long)stats.write_stalls);
    *previous = stats;
}
static void report_rtlsdr_frequency(SoapySDRDevice *dev) {
    double hz = SoapySDRDevice_getFrequency(dev, SOAPY_SDR_RX, 0);
    fprintf(stderr, "[SDR] RTL-SDR tuner readback: %.0f Hz\n",
            isfinite(hz) && hz > 0 ? hz : 0);
}
static double number(const char *s) {
    char *end; errno = 0; double v = strtod(s, &end);
    if (errno || end == s || *end || !isfinite(v)) { fprintf(stderr, "Invalid number: %s\n", s); exit(2); }
    return v;
}
#define U(call) do { if ((call) != UHD_ERROR_NONE) { char msg[1024]; uhd_get_last_error(msg, sizeof(msg)); fprintf(stderr, "%s: %s\n", #call, msg); goto done; } } while (0)
#define S(call) do { if ((call) != 0) { fprintf(stderr, "%s: %s\n", #call, SoapySDRDevice_lastError()); goto done; } } while (0)

static int hackrf_gain_apply(SoapySDRDevice *dev, double requested, int mode, double *actual) {
    HackrfGain g = hackrf_gain(requested);
    /* Keep Auto's former >51 threshold, but never let the aggregate driver
     * redistribute LNA/VGA. 14 is Soapy's AMP on code, not calibrated dB. */
    int amp = mode < 0 ? g.lna + g.vga > 51 : mode;
    /* Lower VGA before raising LNA to avoid a transient high-gain setting. */
    double old_vga = SoapySDRDevice_getGainElement(dev, SOAPY_SDR_RX, 0, "VGA");
    if (g.vga < old_vga && SoapySDRDevice_setGainElement(dev, SOAPY_SDR_RX, 0, "VGA", g.vga)) return -1;
    if (SoapySDRDevice_setGainElement(dev, SOAPY_SDR_RX, 0, "LNA", g.lna) ||
        SoapySDRDevice_setGainElement(dev, SOAPY_SDR_RX, 0, "VGA", g.vga) ||
        SoapySDRDevice_setGainElement(dev, SOAPY_SDR_RX, 0, "AMP", amp ? 14 : 0)) return -1;
    *actual = g.lna + g.vga;
    fprintf(stderr, "[SDR] HackRF gain: LNA-first; LNA %d dB, VGA %d dB (base 12, limit 24); RF amplifier %s (%s)\n",
        g.lna, g.vga, amp ? "on" : "off", mode < 0 ? "auto" : "manual");
    return 0;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    /* CRT argv uses the ANSI code page; control/snapshot paths are UTF-8. */
    argv=g_win32_get_command_line();
#endif
    const char *source = "", *args = "", *images = "", *antenna = "", *format = "s16";
    double rate = 10000000, freq = 437000000, gain = 30, actual = 0;
    double bandwidth = 0, receiver_offset = 0, channel_bandwidth = 0;
    const char *ip = "192.168.0.118", *bind_ip = "0.0.0.0";
    double port = 50005;
    int control_stdin = 0, dab_lo=0, hackrf_amp=-1;
    double dab_if=0;
    const char *b210_port="", *lo_port="TRX1", *dab_state_dir="";
    B210Mixer mixer={0};
    MoonTracker moon={0};
    IQRecord capture = {0};
    TuneControl control = {.command=iq_record_command, .command_data=&capture};
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--dab-lo-output")) {dab_lo=1;continue;}
        if (!strcmp(argv[i], "--control-stdin")) { control_stdin = 1; continue; }
        if (!strcmp(argv[i], "--help")) {
            fprintf(stderr, "sdr_to_stdout --source hackrf|b210|sdriq|afedri|rtlsdr|fcdpp|lime|rsp1a|rsp1b|airspyhf|airspyr2|pluto --sample-rate Hz --frequency Hz --gain dB --format s16|u8 [--ip IPv4 --port TCP_UDP_PORT --bind IPv4] [--args device_args] [--hackrf-amp auto|off|on] [--images DIR] [--antenna TX/RX|RX2|AUTO|LNAW|LNAH] [--b210-input TRX1|RX1|TRX2|RX2] [--dab-if Hz --dab-lo-output --dab-lo-port TRX1|TRX2] [--control-stdin] [--filter-bandwidth Hz] [--receiver-offset Hz --channel-bandwidth Hz]\nHackRF gain: LNA+VGA 12..64 dB, excluding AMP; LNA first, VGA base 12 and limit 24.\nPositive receiver offset selects above the SDR center; 0 bypasses mixing.\nControl stdin: frequency Hz, gain DB, bandwidth HZ (0 = bypass), record FREQUENCY_HZ BASENAME, or record-stop.\n"); return 0;
        }
        if (i + 1 == argc) { fprintf(stderr, "Missing option value\n"); return 2; }
        const char *key = argv[i++], *v = argv[i];
        if (!strcmp(key, "--source")) source = v;
        else if (!strcmp(key, "--hackrf-amp")) {
            if (!strcmp(v,"auto")) hackrf_amp=-1;
            else if (!strcmp(v,"off")) hackrf_amp=0;
            else if (!strcmp(v,"on")) hackrf_amp=1;
            else { fprintf(stderr,"Invalid HackRF amplifier mode: use auto, off or on\n"); return 2; }
        }
        else if (!strcmp(key,"--dab-state-dir")) dab_state_dir=v;
        else if (!strcmp(key,"--dab-if")) dab_if=number(v);
        else if (!strcmp(key,"--b210-input")) b210_port=v;
        else if (!strcmp(key,"--dab-lo-port")) lo_port=v;
        else if (!strcmp(key, "--ip")) ip = v;
        else if (!strcmp(key, "--bind")) bind_ip = v;
        else if (!strcmp(key, "--port")) port = number(v);
        else if (!strcmp(key, "--args")) args = v;
        else if (!strcmp(key, "--images")) images = v;
        else if (!strcmp(key, "--antenna")) antenna = v;
        else if (!strcmp(key, "--moon-track")) { if (!moon_tracker_parse(&moon,v)) return 2; }
        else if (!strcmp(key, "--format")) format = v;
        else if (!strcmp(key, "--receiver-offset")) receiver_offset = number(v);
        else if (!strcmp(key, "--channel-bandwidth")) channel_bandwidth = number(v);
        else if (!strcmp(key, "--filter-bandwidth")) bandwidth = number(v);
        else if (!strcmp(key, "--sample-rate")) rate = number(v);
        else if (!strcmp(key, "--frequency")) freq = number(v);
        else if (!strcmp(key, "--gain")) gain = number(v);
        else { fprintf(stderr, "Unknown option: %s\n", key); return 2; }
    }
    size_t channel=0;
    if ((*b210_port || dab_if || dab_lo) && strcmp(source,"b210")) {
        fprintf(stderr,"B210 input / DAB mixer options require --source b210\n");return 2;
    }
    if (*b210_port && b210_input(b210_port,&channel,&antenna)) {fprintf(stderr,"Unknown B210 physical input\n");return 2;}
    if ((dab_if || dab_lo) && (!b210_mixer_valid(freq,dab_if,b210_port,lo_port,dab_lo) || receiver_offset)) {
        fprintf(stderr,"Invalid DAB mixer IF, RF or physical ports; LO and IF input must be separate\n");return 2;
    }
    int rsp = rx_rsp(source), airspyhf = rx_airspyhf(source), airspyr2 = rx_airspyr2(source);
    int pluto = !strcmp(source, "pluto");
    int lime = !strcmp(source, "lime");
    if (!*antenna) antenna = lime ? "AUTO" : "TX/RX";
    int fcdpp = !strcmp(source, "fcdpp");
    int afedri = !strcmp(source, "afedri");
    int rtlsdr = !strcmp(source, "rtlsdr");
    int sdriq = !strcmp(source, "sdriq");
    int hackrf = !strcmp(source, "hackrf"), u8 = !strcmp(format, "u8");
    int soapy = hackrf || sdriq || rtlsdr || rsp || airspyhf || airspyr2;
    int float_stream = sdriq || rsp || airspyhf;
    if ((!pluto && !airspyr2 && !rsp && !airspyhf && !lime && !fcdpp && !hackrf && !sdriq && !afedri && !rtlsdr && strcmp(source, "b210")) || (strcmp(format, "s16") && !u8) ||
        (pluto ? (rate < 2083334 || rate > 61440000 || rate != floor(rate) || !rx_frequency_valid(source,freq) || gain < 0 || gain > 73) :
         airspyr2 ? (!rx_airspyr2_rate_valid(rate) || !rx_frequency_valid(source,freq) || gain < 0 || gain > 45 || gain != floor(gain)) :
         airspyhf ? (rate < 1000 || rate > 768000 || rate != floor(rate) || !rx_frequency_valid(source,freq) || gain != 0) :
         rsp ? (rate < 62500 || rate > 10000000 || rate != floor(rate) || !rx_frequency_valid(source,freq) || gain < 20 || gain > 59 || gain != floor(gain)) :
         lime ? !lime_rx_valid(rate, freq, gain) : fcdpp ? (rate != FUNCUBE_RATE || !funcube_frequency_valid(freq) || !funcube_gain_valid(gain)) :
         rtlsdr ? (!rtlsdr_rate_valid(rate) || freq < RTLSDR_MIN_FREQUENCY ||
                    freq > RTLSDR_MAX_FREQUENCY || gain < 0 || gain > RTLSDR_MAX_GAIN) : afedri ? (rate < 48000 || rate > 2400000 || rate != floor(rate) ||
                   freq < 0 || freq > 35000000 || !afedri_gain_valid(gain) ||
                   port < 1 || port > 65535 || port != floor(port)) : sdriq ? (!sdriq_rate_valid(rate) || freq < 500 || freq > 30000000 || !sdriq_gain_valid(gain)) :
         (rate < 2000000 || rate > (hackrf ? 20000000 : 61440000) ||
          freq < (hackrf ? 1000000 : 70000000) || freq > 6000000000.0 || gain < (hackrf ? HACKRF_VGA_BASE : 0) || gain > (hackrf ? HACKRF_GAIN_MAX : 76)))) {
        fprintf(stderr, "Invalid source, format, rate, frequency or gain\n");
        if (airspyr2) fprintf(stderr,"Airspy R2: frequency 24000000..1700000000 Hz; rate 2500000 or 10000000; gain setting 0..45 in whole steps\n");
        if (airspyhf) fprintf(stderr,"Airspy HF+ Dual Port VHF: frequency 64000000..260000000 Hz; rate from device (default 768000); automatic gain (gain=0)\n");
        if (rsp) fprintf(stderr,"SDRplay RSP1A/B: frequency 1000..2000000000 Hz; rate 62500..10000000; IF gain reduction 20..59 dB\n");
        if (lime) fprintf(stderr, "LimeSDR Mini: rate 100000..30720000; frequency 10000000..3500000000 Hz; LimeSuite RX gain 0..73 in 1 dB steps\n");
        if (fcdpp) fprintf(stderr, "FUNcube Pro+: fixed rate 192000; frequency 150000..240000000 or 420000000..1900000000 Hz; IF gain 0..59 dB in 1 dB steps\n");
        if (afedri) fprintf(stderr, "AFEDRI: rate 48000..2400000; frequency 0..35000000 Hz; gain -10..35 dB in 3 dB steps; IPv4 and TCP/UDP port required\n");
        if (rtlsdr) fprintf(stderr, "RTL-SDR: rates 225001..300000 or 900001..3200000; frequency 24000000..1764000000 Hz; gain 0..49.6 dB (R820T/R828D tuner mode)\n");
        if (sdriq) fprintf(stderr, "SDR-IQ: rates 8138, 16276, 37793, 55556, 111111, 158730, 196078; frequency 500..30000000 Hz; ATT -20, -10, 0, 10 dB\n");
        return 2;
    }
    if (!rx_tuning_valid(source,freq,receiver_offset)) {
        fprintf(stderr,"SDR/IF frequency or receiver offset outside model tuning range\n"); return 2;
    }
    if (!iq_bandfilter_valid(rate, bandwidth)) {
        fprintf(stderr, "Invalid filter width: use 0 (off), or max(10, sample-rate/256)..0.95*sample-rate Hz\n"); return 2;
    }
    IQShift shift;
    if (iq_shift_init(&shift, rate, receiver_offset) ||
        !rx_bandwidth_fits(source, rate, receiver_offset, channel_bandwidth) ||
        !rx_bandwidth_fits(source, rate, receiver_offset, bandwidth)) {
        fprintf(stderr, "Receiver offset/channel/filter does not fit the sample rate (2.5%% edge margin)\n"); return 2;
    }
    struct sigaction sa = {.sa_handler = stop}; sigemptyset(&sa.sa_mask);
    if (control_stdin) {
        int flags = fcntl(STDIN_FILENO, F_GETFL);
        if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
            perror("tuning stdin"); return 1;
        }
    }
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL); signal(SIGPIPE, SIG_IGN);
    uhd_usrp_handle usrp = NULL; uhd_rx_streamer_handle rx = NULL; uhd_rx_metadata_handle md = NULL;
    PlutoSource *pluto_rx = NULL;
    LimeRx *mini = NULL;
    Afedri *net = NULL;
    Funcube *cube = NULL;
    SoapySDRDevice *dev = NULL; SoapySDRStream *stream = NULL;
    int result = 1, started = 0;
    IQOutput *output = NULL;
    IQBandFilter *filter = iq_bandfilter_create(rate, bandwidth);
    if (!filter) { fprintf(stderr, "Cannot create IQ bandfilter\n"); return 1; }
    unsigned long overflows = 0;
    SdrRxTiming rx_timing = {0};
    int spectrum_async = 0;

    /* Blocks are kept large to amortize pipe and driver overhead at 10+ MS/s. */
    int16_t iq[65536 * 2]; unsigned char packed[65536 * 2];
    float float_iq[2048 * 2];
    PlutoIQ pluto_iq[65536];
    /* About 5 ms of Afedri IQ per DSP/audio block, at most one FIR block.
     * A 16k FFT for each 256-sample datagram otherwise costs ~20x as much. */
    size_t block_samples = lime ? (size_t)fmax(256, fmin(65536, ceil(rate/100/256)*256)) : fcdpp ? 2048 : afedri ? (size_t)fmax(256, fmin(8192, ceil(rate/200/256)*256)) : float_stream ? 2048 : rtlsdr ? 8192 : 65536;
    float *shifted=(receiver_offset != 0 || moon.tx) ? malloc(block_samples*2*sizeof(float)) : NULL;
    if ((receiver_offset != 0 || moon.tx) && !shifted) { fprintf(stderr,"Cannot allocate DVB offset buffer\n"); goto done; }
    if (pluto) {
        const char *uri = !strncmp(args,"uri=",4) ? args+4 : args;
        PlutoConfig config = {.pluto_uri=uri, .ref="internal", .rate=rate,
            .bandwidth=fmin(rate,56000000), .frequency=freq, .gain=gain};
        pluto_rx = pluto_source_open(&config);
        if (!pluto_rx) goto done;
        actual = config.rate; gain = config.gain;
        if (fabs(config.frequency-freq)>2) { fprintf(stderr,"Pluto RX frequency coerced\n"); goto done; }
    } else if (lime) {
        mini = lime_rx_open(args, antenna, rate, freq, &gain, &actual);
        if (!mini) goto done;
    } else if (fcdpp) {
        cube = funcube_open(args, freq, gain);
        if (!cube) goto done;
        actual = FUNCUBE_RATE;
    } else if (afedri) {
        net = afedri_open(ip, (int)port, bind_ip, rate, freq, gain, &actual);
        if (!net) goto done;
    } else if (soapy) {
        if (rsp || airspyhf || airspyr2) {
            const char *driver=airspyr2 ? "airspy" : airspyhf ? "airspyhf" : "sdrplay";
            SoapySDRKwargs selection=SoapySDRKwargs_fromString(args);
            const char *requested_driver=SoapySDRKwargs_get(&selection,"driver");
            if (requested_driver && *requested_driver && strcmp(requested_driver,driver)) {
                fprintf(stderr,"%s requires driver=%s\n",source,driver);
                SoapySDRKwargs_clear(&selection); goto done;
            }
            SoapySDRKwargs_set(&selection,"driver",driver);
            dev=SoapySDRDevice_make(&selection);
            SoapySDRKwargs_clear(&selection);
        } else dev = SoapySDRDevice_makeStrArgs(*args ? args :
            (rtlsdr ? "driver=rtlsdr" : sdriq ? "driver=rfspace,sdr-iq=/dev/ttyUSB0" : "driver=hackrf"));
        if (!dev) { fprintf(stderr, "%s: %s\n", source, SoapySDRDevice_lastError()); goto done; }
        if (rsp || airspyhf || airspyr2) {
            char *model=SoapySDRDevice_getHardwareKey(dev);
            const char *expected=airspyr2 ? "Airspy" : airspyhf ? "AirspyHF" : !strcmp(source,"rsp1a") ? "RSP1A" : "RSP1B";
            int matches=model && !strcmp(model,expected);
            fprintf(stderr,"[SDR] Selected %s; detected %s\n",source,model ? model : "unknown");
            SoapySDR_free(model);
            if (!matches) { fprintf(stderr,"Wrong SDR model for the selected input; select the matching model/serial\n"); goto done; }
            size_t count=0;
            SoapySDRRange *ranges=SoapySDRDevice_getFrequencyRange(dev,SOAPY_SDR_RX,0,&count);
            int valid=0;
            for (size_t i=0;i<count;++i)
                if (freq >= ranges[i].minimum && freq <= ranges[i].maximum &&
                    freq+receiver_offset >= ranges[i].minimum && freq+receiver_offset <= ranges[i].maximum) valid=1;
            SoapySDR_free(ranges);
            if (!valid) { fprintf(stderr,"SDR/IF frequency outside connected device range\n"); goto done; }
            if (airspyhf || airspyr2) {
                double *rates=SoapySDRDevice_listSampleRates(dev,SOAPY_SDR_RX,0,&count);
                valid=0;
                for (size_t i=0;i<count;++i) if (fabs(rates[i]-rate)<.5) valid=1;
                if (!valid) {
                    fprintf(stderr,"Unsupported %s sample rate %.0f; available:",source,rate);
                    for (size_t i=0;i<count;++i) fprintf(stderr," %.0f",rates[i]);
                    fprintf(stderr,"\n");
                }
                SoapySDR_free(rates);
                if (!valid) goto done;
            }
        }
        S(SoapySDRDevice_setSampleRate(dev, SOAPY_SDR_RX, 0, rate));
        actual = SoapySDRDevice_getSampleRate(dev, SOAPY_SDR_RX, 0);
        if (rsp) S(SoapySDRDevice_setBandwidth(dev, SOAPY_SDR_RX, 0, rate));
        S(SoapySDRDevice_setFrequency(dev, SOAPY_SDR_RX, 0, freq, NULL));
        if (rtlsdr) report_rtlsdr_frequency(dev);
        if (airspyr2) {
            /* SoapyAirspy exposes raw LNA/MIX/VGA steps as aggregate gain.
             * It has no settable analog bandwidth; the sample rate sets it. */
            S(SoapySDRDevice_setGainMode(dev, SOAPY_SDR_RX, 0, false));
            S(SoapySDRDevice_setGain(dev, SOAPY_SDR_RX, 0, gain));
            gain=SoapySDRDevice_getGain(dev, SOAPY_SDR_RX, 0);
        } else if (airspyhf) {
            S(SoapySDRDevice_setGainMode(dev, SOAPY_SDR_RX, 0, true));
        } else if (rsp) {
            S(SoapySDRDevice_setGainMode(dev, SOAPY_SDR_RX, 0, false));
            /* RFGR=0 is valid across the supported bands of both models.
             * Expose IF reduction explicitly instead of aggregate "gain". */
            S(SoapySDRDevice_setGainElement(dev, SOAPY_SDR_RX, 0, "RFGR", 0));
            S(SoapySDRDevice_setGainElement(dev, SOAPY_SDR_RX, 0, "IFGR", gain));
            gain=SoapySDRDevice_getGainElement(dev,SOAPY_SDR_RX,0,"IFGR");
        } else if (hackrf) {
            S(SoapySDRDevice_setBandwidth(dev, SOAPY_SDR_RX, 0, rate));
            S(hackrf_gain_apply(dev, gain, hackrf_amp, &gain));
        } else if (sdriq) {
            S(SoapySDRDevice_setGainElement(dev, SOAPY_SDR_RX, 0, "ATT", gain));
            gain = SoapySDRDevice_getGainElement(dev, SOAPY_SDR_RX, 0, "ATT");
        } else {
            if (rtlsdr) S(SoapySDRDevice_setGainMode(dev, SOAPY_SDR_RX, 0, false));
            S(SoapySDRDevice_setBandwidth(dev, SOAPY_SDR_RX, 0, rate));
            S(SoapySDRDevice_setGain(dev, SOAPY_SDR_RX, 0, gain));
            gain = SoapySDRDevice_getGain(dev, SOAPY_SDR_RX, 0);
        }
        /* RTL's default 262144-byte USB buffer adds over half a second at
         * 250 kS/s. Keep audio monitoring and tuning responsive at low rates. */
        SoapySDRKwargs stream_args = {0};
        if (rtlsdr) {
            SoapySDRKwargs_set(&stream_args, "bufflen", "16384");
            /* Retuning can block the control thread briefly. Preserve half a
             * second of ring capacity despite using smaller USB transfers. */
            char buffers[32];
            snprintf(buffers, sizeof(buffers), "%.0f", fmax(15, ceil(rate*.5/8192)));
            SoapySDRKwargs_set(&stream_args, "buffers", buffers);
        }
        const char *stream_format = float_stream ? SOAPY_SDR_CF32 : (u8 && !airspyr2 ? SOAPY_SDR_CS8 : SOAPY_SDR_CS16);
#if SOAPY_SDR_API_VERSION >= 0x00080000
        stream = SoapySDRDevice_setupStream(dev, SOAPY_SDR_RX, stream_format, &channel, 1, &stream_args);
#else
        int stream_result = SoapySDRDevice_setupStream(dev, &stream, SOAPY_SDR_RX, stream_format, &channel, 1, &stream_args);
        if (stream_result != 0) stream = NULL;
#endif
        SoapySDRKwargs_clear(&stream_args);
        if (!stream) { fprintf(stderr, "setupStream: %s\n", SoapySDRDevice_lastError()); goto done; }
    } else {
        if (*images) {
            const char *files[] = {"usrp_b210_fpga.bin", "usrp_b200_fw.hex"};
            for (int i = 0; i < 2; ++i) {
                char path[4096]; int n = snprintf(path, sizeof(path), "%s/%s", images, files[i]);
                if (n < 0 || n >= (int)sizeof(path) ||
#ifdef _WIN32
                    g_access(path, R_OK)
#else
                    access(path, R_OK)
#endif
                    ) {
                    fprintf(stderr, "Kintex-7 image missing: %s/%s\n", images, files[i]); goto done;
                }
            }
            if (setenv("UHD_IMAGES_DIR", images, 1)) { perror("UHD_IMAGES_DIR"); goto done; }
        }
        fprintf(stderr,"[B210 Kintex-7] Opening UHD receiver; images=%s; args=%s\n", *images ? images : "UHD default", args);
        fflush(stderr);
        U(uhd_usrp_make(&usrp, args));
        fprintf(stderr,"[B210 Kintex-7] Device opened; configuring RX channel %zu\n", channel);
        fflush(stderr);
        U(uhd_usrp_set_clock_source(usrp, "internal", 0));
        U(uhd_usrp_set_rx_rate(usrp, rate, channel));
        U(uhd_usrp_get_rx_rate(usrp, channel, &actual));
        U(uhd_usrp_set_rx_bandwidth(usrp, rate, channel));
        U(uhd_usrp_set_rx_antenna(usrp, antenna, channel));
        uhd_tune_request_t tune = {.target_freq=dab_if ? dab_if : freq, .rf_freq_policy=UHD_TUNE_REQUEST_POLICY_AUTO,
            .dsp_freq_policy=UHD_TUNE_REQUEST_POLICY_AUTO, .args=""};
        uhd_tune_result_t tuned;
        U(uhd_usrp_set_rx_freq(usrp, &tune, channel, &tuned));
        U(uhd_usrp_set_rx_gain(usrp, gain, channel, ""));
        U(uhd_usrp_get_rx_gain(usrp, channel, "", &gain));
        if (dab_lo && b210_mixer_open(&mixer,usrp,lo_port,freq,dab_if)) {
            fprintf(stderr,"Cannot configure B210 mixer LO\n");goto done;
        }
        U(uhd_usrp_get_rx_rate(usrp,channel,&actual));
        double tuned_rx=0;U(uhd_usrp_get_rx_freq(usrp,channel,&tuned_rx));
        if (fabs(tuned_rx-(dab_if ? dab_if : freq))>2) {fprintf(stderr,"B210 RX frequency coerced\n");goto done;}
        U(uhd_rx_streamer_make(&rx)); U(uhd_rx_metadata_make(&md));
        uhd_stream_args_t config = {.cpu_format="sc16", .otw_format="sc16", .args="", .channel_list=&channel, .n_channels=1};
        U(uhd_usrp_get_rx_stream(usrp, &config, rx));
    }
    /* The downstream decoder uses the requested rate. Never silently feed a coerced rate. */
    /* SDR-IQ presets are integer-rounded; allow only that rounding difference. */
    if (!isfinite(actual) || fabs(actual-rate) > (sdriq || afedri ? 0.5 : rate*1e-6)) {
        fprintf(stderr, "Requested rate %.0f, hardware rate %.0f; select the hardware rate in the GUI\n", rate, actual); goto done;
    }
    fprintf(stderr, "[SDR] %s: %.0f Hz, %.0f samples/s, gain %.1f %s, %s IQ\n", source, freq, actual, gain, airspyr2 ? "(setting)" : "dB", format);
    if (receiver_offset != 0) fprintf(stderr, "[SDR] Receiver offset %+.0f Hz: hardware %.0f Hz, channel %.0f Hz; spectrum and recording remain at hardware center\n", receiver_offset, freq, freq+receiver_offset);
    size_t slots = (size_t)ceil(rate * 0.5 / block_samples);
    output = iq_output_create(STDOUT_FILENO, block_samples * (u8 ? 2 : 4), slots);
    if (!output) { perror("IQ output queue"); goto done; }
    fprintf(stderr, "[SDR] Independent IQ writer: %.0f ms buffer, %.1f MiB\n",
            slots * (double)block_samples / rate * 1000, slots * (double)block_samples * (u8 ? 2 : 4) / 1048576);
    fprintf(stderr,"[SDR] IQ writer: interruptible backpressure wait\n");
    spectrum_async = iq_monitor_start_async();
    fprintf(stderr, "[SDR] Spectrum processing: %s\n", spectrum_async>0 ? "asynchronous snapshots" :
            spectrum_async<0 ? "synchronous (worker unavailable)" : "synchronous or disabled");
    if (lime) { if (lime_rx_start(mini)) goto done; }
    else if (afedri) { if (afedri_start(net)) goto done; }
    else if (soapy) { S(SoapySDRDevice_activateStream(dev, stream, 0, 0, 0)); }
    else if (!fcdpp && !pluto) { uhd_stream_cmd_t cmd = {.stream_mode=UHD_STREAM_MODE_START_CONTINUOUS, .stream_now=true}; U(uhd_rx_streamer_issue_stream_cmd(rx, &cmd)); }
    started = 1;
    if (dab_lo) {
        int index=dab_channel_nearest(freq), direction=index>=2 ? -1 : 1;
        int path[]={index,index+direction,index+2*direction,index+direction,index};
        for (unsigned p=0;p<sizeof(path)/sizeof(path[0]) && !stopped;++p) {
            // Off-grid DAB must finish on the requested RF, never the nearest
            // Band III preset. Retain the established channel sweep for presets.
            double conditioning_rf=freq==dab_channels[index].hz ? dab_channels[path[p]].hz : freq;
            if (!b210_mixer_valid(conditioning_rf,dab_if,b210_port,lo_port,1)) conditioning_rf=freq;
            if (b210_mixer_tune(&mixer,usrp,conditioning_rf,dab_if)) goto done;
            double until=monotonic_seconds()+.25;
            while (monotonic_seconds()<until && !stopped) {
                size_t got=0;void *buffers[]={iq};
                U(uhd_rx_streamer_recv(rx,buffers,65536,&md,.1,false,&got));
                if (atomic_load(&mixer.failed)) goto done;
            }
        }
        fprintf(stderr,"B210 mixer full-duplex startup conditioning complete\n");
    }
    double last_report = monotonic_seconds();
    double last_rx_report = last_report;
    IQOutputStats previous = {0};
    capture.rate = actual;
    uint64_t dab_generation=0;
    size_t dab_settle=0;
    while (!stopped) {
        /* Runs even after timeout/overflow continuations: loss diagnostics must
         * not depend on successfully enqueuing a block. Currently UHD only. */
        double loop_now = monotonic_seconds();
        if (rx_timing.status.calls && loop_now-last_rx_report>=1) {
            IQOutputStats stats; iq_output_stats(output,&stats);
            sdr_rx_report(stderr,&rx_timing,actual,loop_now-last_rx_report,
                stats.dropped/(u8 ? 2 : 4),iq_monitor_dropped(),spectrum_async>0);
            last_rx_report=loop_now;
        }
        if (dab_lo && atomic_load(&mixer.failed)) goto done;
        iq_record_feed(&capture, NULL, 0, 0);
        double requested = freq, requested_gain = gain, requested_bw = bandwidth;
        int changed = control_stdin ? tune_control_read(&control, STDIN_FILENO,
                rx_frequency_min(source), rx_frequency_max(source), &requested,
                rsp ? 20 : afedri ? -10 : sdriq ? -20 : hackrf ? HACKRF_VGA_BASE : 0,
                pluto ? 73 : airspyr2 ? 45 : airspyhf ? 0 : rsp ? 59 : lime ? LIME_RX_MAX_GAIN : fcdpp ? FUNCUBE_MAX_GAIN : rtlsdr ? RTLSDR_MAX_GAIN : afedri ? 35 : sdriq ? 10 : hackrf ? HACKRF_GAIN_MAX : 76,
                pluto || airspyr2 || rsp || lime || fcdpp ? 1 : afedri ? 3 : sdriq ? 10 : 0, &requested_gain, rate, &requested_bw) : 0;
        if ((changed & 1) && (!rx_tuning_valid(source,requested,receiver_offset) ||
            (dab_if && !b210_mixer_valid(requested,dab_if,b210_port,lo_port,dab_lo)))) {
            fprintf(stderr, "[SDR] Ignoring tune outside model/input range\n");
            changed &= ~1;
        }
        if ((changed & 1) && (requested != freq || (control.tune_generation && *dab_state_dir))) {
            if (pluto) {
                double tuned = requested;
                if (pluto_source_tune(pluto_rx, &tuned, &gain) || fabs(tuned-requested)>2) goto done;
            } else if (lime) {
                if (lime_rx_frequency(mini, requested)) goto done;
            } else if (fcdpp) {
                if (funcube_frequency(cube, requested)) goto done;
            } else if (afedri) {
                if (afedri_frequency(net, requested)) goto done;
            } else if (dev) {
                S(SoapySDRDevice_setFrequency(dev, SOAPY_SDR_RX, 0, requested, NULL));
                if (rtlsdr) report_rtlsdr_frequency(dev);
            } else if (!dab_if) {
                uhd_tune_request_t tune = {.target_freq=requested, .rf_freq_policy=UHD_TUNE_REQUEST_POLICY_AUTO,
                    .dsp_freq_policy=UHD_TUNE_REQUEST_POLICY_AUTO, .args=""};
                uhd_tune_result_t tuned;
                U(uhd_usrp_set_rx_freq(usrp, &tune, channel, &tuned));
            }
            if (dab_lo && b210_mixer_tune(&mixer,usrp,requested,dab_if)) goto done;
            if (moon.tx) { moon.center += requested-freq; moon.next_update=-INFINITY; }
            freq = requested;
            if (*dab_state_dir && control.tune_generation) {
                dab_generation=control.tune_generation;
                dab_settle=(size_t)(rate*.05); /* discard tuner/driver settling IQ, keep streaming */
                iq_bandfilter_destroy(filter);filter=iq_bandfilter_create(rate,bandwidth);
                if (!filter) goto done;
            }
            fprintf(stderr, "[SDR] Tuned to %.0f Hz; decoder may need to reacquire\n", freq);
        }
        if ((changed & 2) && requested_gain != gain) {
            if (pluto) {
                double tuned = freq;
                gain = requested_gain;
                if (pluto_source_tune(pluto_rx, &tuned, &gain)) goto done;
            } else if (lime) {
                double actual_gain = requested_gain;
                if (lime_rx_gain(mini, &actual_gain)) goto done;
                gain = actual_gain;
            } else if (fcdpp) {
                if (funcube_gain(cube, requested_gain)) goto done;
                gain = requested_gain;
            } else if (afedri) {
                if (afedri_gain(net, requested_gain)) goto done;
                gain = requested_gain;
            } else if (rsp) {
                S(SoapySDRDevice_setGainElement(dev, SOAPY_SDR_RX, 0, "IFGR", requested_gain));
                gain=SoapySDRDevice_getGainElement(dev,SOAPY_SDR_RX,0,"IFGR");
            } else if (sdriq) {
                S(SoapySDRDevice_setGainElement(dev, SOAPY_SDR_RX, 0, "ATT", requested_gain));
                gain = SoapySDRDevice_getGainElement(dev, SOAPY_SDR_RX, 0, "ATT");
            } else if (hackrf) {
                S(hackrf_gain_apply(dev, requested_gain, hackrf_amp, &gain));
            } else if (rtlsdr || airspyr2) {
                S(SoapySDRDevice_setGain(dev, SOAPY_SDR_RX, 0, requested_gain));
                gain = SoapySDRDevice_getGain(dev, SOAPY_SDR_RX, 0);
            } else {
                U(uhd_usrp_set_rx_gain(usrp, requested_gain, channel, ""));
                U(uhd_usrp_get_rx_gain(usrp, channel, "", &gain));
            }
            fprintf(stderr, "[SDR] RX gain %.1f %s (requested %.1f)\n", gain, airspyr2 ? "(setting)" : "dB", requested_gain);
        }
        if ((changed & 4) && !rx_bandwidth_fits(source, rate, receiver_offset, requested_bw)) {
            fprintf(stderr, "[SDR] Ignoring filter width: receiver offset + filter exceeds source band\n");
            changed &= ~4;
        }
        if ((changed & 4) && requested_bw != bandwidth) {
            if (iq_bandfilter_set(filter, requested_bw)) { fprintf(stderr, "Cannot update IQ bandfilter\n"); goto done; }
            bandwidth = requested_bw;
            fprintf(stderr, "[SDR] IQ filter width %.0f Hz (0=bypass); sample rate unchanged\n", bandwidth);
        }
        int output_error = iq_output_error(output);
        if (output_error) { fprintf(stderr, "IQ output: %s\n", strerror(output_error)); goto done; }
        size_t got = 0; void *buffers[] = {float_stream ? (void *)float_iq : ((hackrf || rtlsdr) && u8 ? (void *)packed : (void *)iq)};
        if (pluto) {
            int n = pluto_source_read(pluto_rx, pluto_iq, block_samples);
            if (n == -2) { ++overflows; continue; }
            if (n < 0) goto done;
            if (!n) continue;
            got = (size_t)n;
            for (size_t k=0;k<got;++k) {
                iq[2*k]=(int16_t)fmax(-32768,fmin(32767,pluto_iq[k].i*32768));
                iq[2*k+1]=(int16_t)fmax(-32768,fmin(32767,pluto_iq[k].q*32768));
            }
        } else if (lime) {
            int n = lime_rx_read(mini, iq, block_samples);
            if (n < 0) goto done;
            if (!n) { if (lime_rx_report(mini)) goto done; continue; }
            got = (size_t)n;
        } else if (fcdpp) {
            int n = funcube_read(cube, iq, block_samples);
            if (n < 0) goto done;
            if (!n) continue;
            got = (size_t)n;
        } else if (afedri) {
            int n = afedri_read(net, iq, block_samples);
            if (n < 0) goto done;
            if (!n) continue;
            got = (size_t)n;
        } else if (soapy) {
            int flags = 0; long long time = 0;
            int n = SoapySDRDevice_readStream(dev, stream, buffers, block_samples, &flags, &time, 100000);
            if (n == SOAPY_SDR_TIMEOUT) continue;
            if (n == SOAPY_SDR_OVERFLOW) {
                ++overflows;
                if (overflows == 1 || !(overflows & (overflows-1)))
                    fprintf(stderr, "[SDR] Hardware overflow #%lu: samples lost; continuing RX\n", overflows);
                continue;
            }
            if (n < 0) { fprintf(stderr, "SDR receive: %s (decoder/USB may not keep up)\n", SoapySDR_errToStr(n)); goto done; }
            got = (size_t)n;
        } else {
            sdr_rx_begin(&rx_timing,monotonic_seconds());
            U(uhd_rx_streamer_recv(rx, buffers, 65536, &md, .1, false, &got));
            double recv_end = monotonic_seconds();
            uhd_rx_metadata_error_code_t code; U(uhd_rx_metadata_error_code(md, &code));
            sdr_rx_end(&rx_timing,recv_end,got,code==UHD_RX_METADATA_ERROR_CODE_OVERFLOW,
                code==UHD_RX_METADATA_ERROR_CODE_TIMEOUT);
            if (code == UHD_RX_METADATA_ERROR_CODE_TIMEOUT) continue;
            if (code == UHD_RX_METADATA_ERROR_CODE_OVERFLOW) {
                ++overflows;
                if (overflows == 1 || !(overflows & (overflows-1)))
                    fprintf(stderr, "[SDR] Hardware overflow #%lu: samples lost; continuing RX\n", overflows);
                continue;
            }
            if (code != UHD_RX_METADATA_ERROR_CODE_NONE) {
                char msg[512]; uhd_rx_metadata_strerror(md, msg, sizeof(msg));
                fprintf(stderr, "SDR receive: %s (decoder/USB may not keep up)\n", msg); goto done;
            }
        }
        if (dab_settle) {
            if (got<dab_settle) {dab_settle-=got;continue;}
            dab_settle=0;
            /* accepted counts precisely the bytes preceding this retune in the
             * output stream, including queued bytes. No drain/reopen is needed. */
            IQOutputStats boundary;iq_output_stats(output,&boundary);
            char path[4096],ack[128];
            if (snprintf(path,sizeof(path),"%s/tuned",dab_state_dir)>=(int)sizeof(path)) goto done;
            int length=snprintf(ack,sizeof(ack),"%llu %.0f %llu\n",(unsigned long long)dab_generation,freq,(unsigned long long)boundary.accepted);
            int wrote=snapshot_write(path,ack,(size_t)length);
            if (wrote<0) {perror("DAB tuning acknowledgement");goto done;}
            /* Keep discarding IQ until the ack can be published. Its byte
             * boundary remains valid because no new IQ has been enqueued. */
            if (wrote>0) dab_settle=1;
            continue;
        }
        if (float_stream) iq_cf32_to_s16(float_iq, iq, got);
        iq_record_feed(&capture, (hackrf || rtlsdr) && u8 ? (void *)packed : (void *)iq, got, (hackrf || rtlsdr) && u8 ? 2 : 0);
        if (u8) {
            if (hackrf || rtlsdr) iq_cs8_to_u8(packed, got);
            else iq_cs16_to_u8(iq, packed, got);
        }
        unsigned char *out = u8 ? packed : (unsigned char *)iq;
        double monitor_start = monotonic_seconds();
        iq_monitor_feed(out, got, u8, rate);
        rx_timing.status.monitor_max_ms=fmax(rx_timing.status.monitor_max_ms,
            (monotonic_seconds()-monitor_start)*1000);
        if (moon.tx && (!moon_tracker_update(&moon,&shift,rate,g_get_real_time()/1e6-got/rate) ||
            !rx_bandwidth_fits(source,rate,shift.offset,fmax(channel_bandwidth,bandwidth)))) {
            fprintf(stderr,"[moon] Cannot track within the selected SDR bandwidth\n"); goto done;
        }
        if (shifted) {
            /* Raw spectrum/audio/recording are tapped above. Preserve float
             * headroom through mixing + filtering; quantize only at output. */
            for (size_t n=0;n<got*2;++n)
                shifted[n]=u8 && (hackrf || rtlsdr) ? ((int)packed[n]-128)*256 : iq[n];
            iq_shift_process(&shift, shifted, got);
            iq_bandfilter_process_float(filter, shifted, got);
            iq_shift_pack(shifted,out,got,u8);
        } else iq_bandfilter_process(filter, out, got, u8);
        size_t bytes = got * (u8 ? 2 : 4);
        double output_start = monotonic_seconds();
        int queued = iq_output_push(output, out, bytes);
        rx_timing.status.output_max_ms=fmax(rx_timing.status.output_max_ms,
            (monotonic_seconds()-output_start)*1000);
        if (queued < 0) {
            fprintf(stderr, "IQ output: %s\n", strerror(iq_output_error(output)));
            goto done;
        }
        if (queued == 1) {
            IQOutputStats stats; iq_output_stats(output, &stats);
            if (stats.dropped == bytes)
                fprintf(stderr, "[SDR] Decoder output queue full: dropping incoming IQ blocks and continuing RX; samples lost, decoder may need to reacquire. See throughput/drop counters.\n");
        }
        double now = monotonic_seconds();
        if (now-last_report >= 1) {
            report_output(output, rate, u8 ? 2 : 4, now-last_report, &previous, "Throughput");
            if (afedri) afedri_report(net);
            if (lime && lime_rx_report(mini)) goto done;
            last_report = now;
        }
    }
    result = 0;
done:
    iq_monitor_stop_async();
    free(shifted);
    pluto_source_close(pluto_rx);
    lime_rx_close(mini);
    funcube_close(cube);
    afedri_close(net);
    iq_record_stop(&capture);
    if (output) {
        IQOutputStats stats; iq_output_stats(output, &stats);
        fprintf(stderr, "[SDR] Total IQ samples dropped for slow decoder: %llu\n",
                (unsigned long long)(stats.dropped/(u8 ? 2 : 4)));
    }
    iq_output_destroy(output);
    iq_bandfilter_destroy(filter);
    if (overflows) fprintf(stderr, "[SDR] Total hardware overflows: %lu\n", overflows);
    if (started && stream) SoapySDRDevice_deactivateStream(dev, stream, 0, 0);
    if (started && rx) { uhd_stream_cmd_t cmd = {.stream_mode=UHD_STREAM_MODE_STOP_CONTINUOUS, .stream_now=true}; uhd_rx_streamer_issue_stream_cmd(rx, &cmd); }
    if (stream) SoapySDRDevice_closeStream(dev, stream);
    if (dev) SoapySDRDevice_unmake(dev);
    if (md) uhd_rx_metadata_free(&md);
    if (rx) uhd_rx_streamer_free(&rx);
    b210_mixer_close(&mixer);
    if (usrp) uhd_usrp_free(&usrp);
    return stopped ? 0 : result;
}
