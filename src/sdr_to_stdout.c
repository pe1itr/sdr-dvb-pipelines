#define _POSIX_C_SOURCE 200809L
#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Errors.h>
#include <uhd.h>
#include "iq_output.h"
#include "iq_convert.h"
#include "sdriq.h"
#include "tune_control.h"
#include "iq_bandfilter.h"
#include "iq_monitor.h"
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

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
    *previous = stats;
}
static double number(const char *s) {
    char *end; errno = 0; double v = strtod(s, &end);
    if (errno || end == s || *end || !isfinite(v)) { fprintf(stderr, "Invalid number: %s\n", s); exit(2); }
    return v;
}
#define U(call) do { if ((call) != UHD_ERROR_NONE) { char msg[1024]; uhd_get_last_error(msg, sizeof(msg)); fprintf(stderr, "%s: %s\n", #call, msg); goto done; } } while (0)
#define S(call) do { if ((call) != 0) { fprintf(stderr, "%s: %s\n", #call, SoapySDRDevice_lastError()); goto done; } } while (0)

int main(int argc, char **argv) {
    const char *source = "", *args = "", *images = "", *antenna = "TX/RX", *format = "s16";
    double rate = 10000000, freq = 437000000, gain = 30, actual = 0;
    double bandwidth = 0;
    int control_stdin = 0;
    TuneControl control = {0};
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--control-stdin")) { control_stdin = 1; continue; }
        if (!strcmp(argv[i], "--help")) {
            fprintf(stderr, "sdr_to_stdout --source hackrf|b210|sdriq --sample-rate Hz --frequency Hz --gain dB --format s16|u8 [--args device_args] [--images DIR] [--antenna TX/RX|RX2] [--control-stdin] [--filter-bandwidth Hz]\nControl stdin: frequency Hz, gain DB, or bandwidth HZ (0 = bypass).\n"); return 0;
        }
        if (i + 1 == argc) { fprintf(stderr, "Missing option value\n"); return 2; }
        const char *key = argv[i++], *v = argv[i];
        if (!strcmp(key, "--source")) source = v;
        else if (!strcmp(key, "--args")) args = v;
        else if (!strcmp(key, "--images")) images = v;
        else if (!strcmp(key, "--antenna")) antenna = v;
        else if (!strcmp(key, "--format")) format = v;
        else if (!strcmp(key, "--filter-bandwidth")) bandwidth = number(v);
        else if (!strcmp(key, "--sample-rate")) rate = number(v);
        else if (!strcmp(key, "--frequency")) freq = number(v);
        else if (!strcmp(key, "--gain")) gain = number(v);
        else { fprintf(stderr, "Unknown option: %s\n", key); return 2; }
    }
    int sdriq = !strcmp(source, "sdriq");
    int hackrf = !strcmp(source, "hackrf"), u8 = !strcmp(format, "u8");
    if ((!hackrf && !sdriq && strcmp(source, "b210")) || (strcmp(format, "s16") && !u8) ||
        (sdriq ? (!sdriq_rate_valid(rate) || freq < 500 || freq > 30000000 || !sdriq_gain_valid(gain)) :
         (rate < 2000000 || rate > (hackrf ? 20000000 : 61440000) ||
          freq < (hackrf ? 1000000 : 70000000) || freq > 6000000000.0 || gain < 0 || gain > (hackrf ? 116 : 76)))) {
        fprintf(stderr, "Invalid source, format, rate, frequency or gain\n");
        if (sdriq) fprintf(stderr, "SDR-IQ: rates 8138, 16276, 37793, 55556, 111111, 158730, 196078; frequency 500..30000000 Hz; ATT -20, -10, 0, 10 dB\n");
        return 2;
    }
    if (!iq_bandfilter_valid(rate, bandwidth)) {
        fprintf(stderr, "Invalid filter width: use 0 (off), or max(10, sample-rate/256)..0.95*sample-rate Hz\n"); return 2;
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
    SoapySDRDevice *dev = NULL; SoapySDRStream *stream = NULL;
    int result = 1, started = 0;
    IQOutput *output = NULL;
    IQBandFilter *filter = iq_bandfilter_create(rate, bandwidth);
    if (!filter) { fprintf(stderr, "Cannot create IQ bandfilter\n"); return 1; }
    unsigned long overflows = 0;
    size_t channel = 0;
    /* Blocks are kept large to amortize pipe and driver overhead at 10+ MS/s. */
    int16_t iq[65536 * 2]; unsigned char packed[65536 * 2];
    float float_iq[2048 * 2];
    size_t block_samples = sdriq ? 2048 : 65536;
    if (hackrf || sdriq) {
        dev = SoapySDRDevice_makeStrArgs(*args ? args :
            (sdriq ? "driver=rfspace,sdr-iq=/dev/ttyUSB0" : "driver=hackrf"));
        if (!dev) { fprintf(stderr, "%s: %s\n", source, SoapySDRDevice_lastError()); goto done; }
        S(SoapySDRDevice_setSampleRate(dev, SOAPY_SDR_RX, 0, rate));
        actual = SoapySDRDevice_getSampleRate(dev, SOAPY_SDR_RX, 0);
        S(SoapySDRDevice_setFrequency(dev, SOAPY_SDR_RX, 0, freq, NULL));
        if (sdriq) {
            S(SoapySDRDevice_setGainElement(dev, SOAPY_SDR_RX, 0, "ATT", gain));
            gain = SoapySDRDevice_getGainElement(dev, SOAPY_SDR_RX, 0, "ATT");
        } else {
            S(SoapySDRDevice_setBandwidth(dev, SOAPY_SDR_RX, 0, rate));
            S(SoapySDRDevice_setGain(dev, SOAPY_SDR_RX, 0, gain));
            gain = SoapySDRDevice_getGain(dev, SOAPY_SDR_RX, 0);
        }
        stream = SoapySDRDevice_setupStream(dev, SOAPY_SDR_RX, sdriq ? SOAPY_SDR_CF32 : (u8 ? SOAPY_SDR_CS8 : SOAPY_SDR_CS16), &channel, 1, NULL);
        if (!stream) { fprintf(stderr, "setupStream: %s\n", SoapySDRDevice_lastError()); goto done; }
    } else {
        if (*images) {
            const char *files[] = {"usrp_b210_fpga.bin", "usrp_b200_fw.hex"};
            for (int i = 0; i < 2; ++i) {
                char path[4096]; int n = snprintf(path, sizeof(path), "%s/%s", images, files[i]);
                if (n < 0 || n >= (int)sizeof(path) || access(path, R_OK)) {
                    fprintf(stderr, "Kintex-7 image missing: %s/%s\n", images, files[i]); goto done;
                }
            }
            if (setenv("UHD_IMAGES_DIR", images, 1)) { perror("UHD_IMAGES_DIR"); goto done; }
        }
        U(uhd_usrp_make(&usrp, args));
        U(uhd_usrp_set_clock_source(usrp, "internal", 0));
        U(uhd_usrp_set_rx_rate(usrp, rate, 0));
        U(uhd_usrp_get_rx_rate(usrp, 0, &actual));
        U(uhd_usrp_set_rx_bandwidth(usrp, rate, 0));
        U(uhd_usrp_set_rx_antenna(usrp, antenna, 0));
        uhd_tune_request_t tune = {.target_freq=freq, .rf_freq_policy=UHD_TUNE_REQUEST_POLICY_AUTO,
            .dsp_freq_policy=UHD_TUNE_REQUEST_POLICY_AUTO, .args=""};
        uhd_tune_result_t tuned;
        U(uhd_usrp_set_rx_freq(usrp, &tune, 0, &tuned));
        U(uhd_usrp_set_rx_gain(usrp, gain, 0, ""));
        U(uhd_usrp_get_rx_gain(usrp, 0, "", &gain));
        U(uhd_rx_streamer_make(&rx)); U(uhd_rx_metadata_make(&md));
        uhd_stream_args_t config = {.cpu_format="sc16", .otw_format="sc16", .args="", .channel_list=&channel, .n_channels=1};
        U(uhd_usrp_get_rx_stream(usrp, &config, rx));
    }
    /* The downstream decoder uses the requested rate. Never silently feed a coerced rate. */
    /* SDR-IQ presets are integer-rounded; allow only that rounding difference. */
    if (!isfinite(actual) || fabs(actual-rate) > (sdriq ? 0.5 : rate*1e-6)) {
        fprintf(stderr, "Requested rate %.0f, hardware rate %.0f; select the hardware rate in the GUI\n", rate, actual); goto done;
    }
    fprintf(stderr, "[SDR] %s: %.0f Hz, %.0f samples/s, gain %.1f dB, %s IQ\n", source, freq, actual, gain, format);
    size_t slots = (size_t)ceil(rate * 0.5 / block_samples);
    output = iq_output_create(STDOUT_FILENO, block_samples * (u8 ? 2 : 4), slots);
    if (!output) { perror("IQ output queue"); goto done; }
    fprintf(stderr, "[SDR] Independent IQ writer: %.0f ms buffer, %.1f MiB\n",
            slots * (double)block_samples / rate * 1000, slots * (double)block_samples * (u8 ? 2 : 4) / 1048576);
    if (hackrf || sdriq) { S(SoapySDRDevice_activateStream(dev, stream, 0, 0, 0)); }
    else { uhd_stream_cmd_t cmd = {.stream_mode=UHD_STREAM_MODE_START_CONTINUOUS, .stream_now=true}; U(uhd_rx_streamer_issue_stream_cmd(rx, &cmd)); }
    started = 1;
    double last_report = monotonic_seconds();
    IQOutputStats previous = {0};
    while (!stopped) {
        double requested = freq, requested_gain = gain, requested_bw = bandwidth;
        int changed = control_stdin ? tune_control_read(&control, STDIN_FILENO,
                sdriq ? 500 : hackrf ? 1000000 : 70000000,
                sdriq ? 30000000 : 6000000000.0, &requested,
                sdriq ? -20 : 0, sdriq ? 10 : hackrf ? 116 : 76,
                sdriq ? 10 : 0, &requested_gain, rate, &requested_bw) : 0;
        if ((changed & 1) && requested != freq) {
            if (dev) {
                S(SoapySDRDevice_setFrequency(dev, SOAPY_SDR_RX, 0, requested, NULL));
            } else {
                uhd_tune_request_t tune = {.target_freq=requested, .rf_freq_policy=UHD_TUNE_REQUEST_POLICY_AUTO,
                    .dsp_freq_policy=UHD_TUNE_REQUEST_POLICY_AUTO, .args=""};
                uhd_tune_result_t tuned;
                U(uhd_usrp_set_rx_freq(usrp, &tune, 0, &tuned));
            }
            freq = requested;
            fprintf(stderr, "[SDR] Tuned to %.0f Hz; decoder may need to reacquire\n", freq);
        }
        if ((changed & 2) && requested_gain != gain) {
            if (sdriq) {
                S(SoapySDRDevice_setGainElement(dev, SOAPY_SDR_RX, 0, "ATT", requested_gain));
                gain = SoapySDRDevice_getGainElement(dev, SOAPY_SDR_RX, 0, "ATT");
            } else if (hackrf) {
                S(SoapySDRDevice_setGain(dev, SOAPY_SDR_RX, 0, requested_gain));
                gain = SoapySDRDevice_getGain(dev, SOAPY_SDR_RX, 0);
            } else {
                U(uhd_usrp_set_rx_gain(usrp, requested_gain, 0, ""));
                U(uhd_usrp_get_rx_gain(usrp, 0, "", &gain));
            }
            fprintf(stderr, "[SDR] RX gain %.1f dB (requested %.1f dB)\n", gain, requested_gain);
        }
        if ((changed & 4) && requested_bw != bandwidth) {
            if (iq_bandfilter_set(filter, requested_bw)) { fprintf(stderr, "Cannot update IQ bandfilter\n"); goto done; }
            bandwidth = requested_bw;
            fprintf(stderr, "[SDR] IQ filter width %.0f Hz (0=bypass); sample rate unchanged\n", bandwidth);
        }
        int output_error = iq_output_error(output);
        if (output_error) { fprintf(stderr, "IQ output: %s\n", strerror(output_error)); goto done; }
        size_t got = 0; void *buffers[] = {sdriq ? (void *)float_iq : (hackrf && u8 ? (void *)packed : (void *)iq)};
        if (hackrf || sdriq) {
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
            U(uhd_rx_streamer_recv(rx, buffers, 65536, &md, .1, false, &got));
            uhd_rx_metadata_error_code_t code; U(uhd_rx_metadata_error_code(md, &code));
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
        if (sdriq) iq_cf32_to_s16(float_iq, iq, got);
        if (u8) {
            if (hackrf) iq_cs8_to_u8(packed, got);
            else iq_cs16_to_u8(iq, packed, got);
        }
        unsigned char *out = u8 ? packed : (unsigned char *)iq;
        iq_monitor_feed(out, got, u8, rate);
        iq_bandfilter_process(filter, out, got, u8);
        size_t bytes = got * (u8 ? 2 : 4);
        int queued = iq_output_push(output, out, bytes);
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
            last_report = now;
        }
    }
    result = 0;
done:
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
    if (usrp) uhd_usrp_free(&usrp);
    return stopped ? 0 : result;
}
