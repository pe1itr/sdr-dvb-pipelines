#define _POSIX_C_SOURCE 200809L
#include "../components/moon/moon.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include "iq_bandfilter.h"
#include "iq_monitor.h"
#include "iq_record.h"
#include "tune_control.h"
#include <poll.h>
#include <errno.h>
#include <getopt.h>
#include <glib.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop_signal(int sig) { (void)sig; stopping = 1; }

typedef struct {
    uint64_t count;
    double sum[2], square[2];
    int min[2], max[2];
} Stats;

static int s16(const uint8_t *p) {
    unsigned v = p[0] | ((unsigned)p[1] << 8);
    return v >= 32768 ? (int)v - 65536 : (int)v;
}

static void measure(Stats *s, const uint8_t *p, size_t n, int u8) {
    size_t stride = u8 ? 2 : 4;
    for (size_t i = 0; i + stride <= n; i += stride) {
        for (int axis = 0; axis < 2; ++axis) {
            int v = u8 ? p[i + axis] : s16(p + i + axis * 2);
            if (!s->count || v < s->min[axis]) s->min[axis] = v;
            if (!s->count || v > s->max[axis]) s->max[axis] = v;
            s->sum[axis] += v;
            s->square[axis] += (double)v * v;
        }
        ++s->count;
    }
}

static void report(const Stats *s, const char *label, int u8) {
    fprintf(stderr, "[%s] type=%s iq_samples=%llu\n", label, u8 ? "u8" : "s16",
            (unsigned long long)s->count);
    if (!s->count) return;
    for (int a = 0; a < 2; ++a) {
        double mean = s->sum[a] / s->count, sq = s->square[a] / s->count;
        fprintf(stderr, "%c:\n  mean = %.3f\n  std  = %.3f\n  rms  = %.3f\n  min  = %d\n  max  = %d\n",
                'I' + a * 8, mean, sqrt(fmax(0, sq - mean * mean)), sqrt(sq), s->min[a], s->max[a]);
    }
}

static double number(const char *text, double low, double high, int integer) {
    char *end;
    errno = 0;
    double v = strtod(text, &end);
    if (errno || end == text || *end || !isfinite(v) || v < low || v > high || (integer && trunc(v) != v)) {
        fprintf(stderr, "Invalid numeric value: %s\n", text);
        exit(2);
    }
    return v;
}

static int write_all(FILE *f, const uint8_t *p, size_t n) {
    while (n && !stopping) {
        size_t k = fwrite(p, 1, n, f);
        if (!k) {
            if (errno == EINTR) { clearerr(f); continue; }
            return -1;
        }
        p += k;
        n -= k;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *ip = "127.0.0.1", *record_dir = NULL, *prefix = "linrad";
    int port = 50000, skip = 24, bufsize = 8192, u8 = 0, offset = 128, verbose = 0, record_stdout = 0;
    double scale = 256, gain = 1, sample_rate = 0, bandwidth = 0;
    int control_stdin = 0;
    MoonTracker moon={0}; IQShift shift={.re=1};
    IQRecord capture = {0};
    TuneControl control = {.command=iq_record_command, .command_data=&capture};
    size_t limit = 0;
    enum { IP = 256, PORT, SKIP, BUFSIZE, U8, SCALE, OFFSET, GAIN, MEASURE, RECORD, PREFIX, STDOUT, VERBOSE, SAMPLE_RATE, FILTER, CONTROL, MOON };
    const struct option opts[] = {
        {"moon-track",1,NULL,MOON},
        {"sample-rate", 1, NULL, SAMPLE_RATE}, {"filter-bandwidth", 1, NULL, FILTER}, {"control-stdin", 0, NULL, CONTROL},
        {"ip", 1, NULL, IP}, {"port", 1, NULL, PORT}, {"skip", 1, NULL, SKIP},
        {"bufsize", 1, NULL, BUFSIZE}, {"u8", 0, NULL, U8}, {"scale", 1, NULL, SCALE},
        {"offset", 1, NULL, OFFSET}, {"s16-gain", 1, NULL, GAIN}, {"measure", 1, NULL, MEASURE},
        {"record-iq", 2, NULL, RECORD}, {"record-prefix", 1, NULL, PREFIX},
        {"record-stdout", 0, NULL, STDOUT}, {"verbose", 0, NULL, VERBOSE}, {"help", 0, NULL, 'h'}, {NULL, 0, NULL, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
        switch (c) {
        case SAMPLE_RATE: sample_rate = number(optarg, 1, 100000000, 1); break;
        case MOON: if (!moon_tracker_parse(&moon,optarg)) return 2; break;
        case FILTER: bandwidth = number(optarg, 0, 100000000, 1); break;
        case CONTROL: control_stdin = 1; break;
        case IP: ip = optarg; break;
        case PORT: port = number(optarg, 1, 65535, 1); break;
        case SKIP: skip = number(optarg, 0, 65535, 1); break;
        case BUFSIZE: bufsize = number(optarg, 1, 65535, 1); break;
        case U8: u8 = 1; break;
        case SCALE: scale = number(optarg, -1e12, 1e12, 0); break;
        case OFFSET: offset = number(optarg, -2147483648.0, 2147483647.0, 1); break;
        case GAIN: gain = number(optarg, -1e12, 1e12, 0); break;
        case MEASURE: limit = (size_t)number(optarg, 0, 1e12, 1); break;
        case RECORD:
            record_dir = optarg ? optarg : (optind < argc && argv[optind][0] != '-' ? argv[optind++] : "recordings");
            break;
        case PREFIX: prefix = optarg; break;
        case STDOUT: record_stdout = 1; break;
        case VERBOSE: verbose = 1; break;
        case 'h':
            puts("Linrad UDP s16 IQ -> stdout (C)\n"
                 "  --ip IP             Listen address (127.0.0.1)\n"
                 "  --port N            UDP port (50000)\n"
                 "  --skip N            Header bytes per packet (24)\n"
                 "  --bufsize N         Maximum datagram size (8192); larger packets are rejected\n"
                 "  --u8                Convert to unsigned 8-bit IQ\n"
                 "  --scale N           u8 = clip(round(offset + s16*scale/256)) (256)\n"
                 "  --offset N          u8 offset (128)\n"
                 "  --s16-gain N        Gain for s16 output (1)\n"
                 "  --measure N         Report first N input/output bytes, rounded down to IQ pairs\n"
                 "  --record-iq [DIR]    Record output IQ (recordings); stdout off unless requested\n"
                 "  --record-prefix S   Recording filename prefix (linrad)\n"
                 "  --record-stdout     Also stream when recording\n"
                 "  --verbose           Packet counters on stderr\n"
                 "  --sample-rate Hz    Required for IQ filtering (no resampling)\n"
                 "  --filter-bandwidth Hz  Total FIR width; 0=bypass\n"
                 "  --control-stdin     Accept bandwidth Hz, record FREQUENCY_HZ BASENAME, record-stop");
            return 0;
        default: return 2;
        }
    }
    if (optind != argc || skip >= bufsize || !*prefix || strchr(prefix, '/')) {
        fprintf(stderr, "Invalid arguments (skip must be smaller than bufsize; prefix must be a filename).\n");
        return 2;
    }
    if ((bandwidth || control_stdin || moon.tx) && !iq_bandfilter_valid(sample_rate, bandwidth)) {
        fprintf(stderr, "IQ filtering requires --sample-rate and a valid width (0, or max(10, Fs/256)..0.95*Fs)\n"); return 2;
    }
    if (control_stdin) {
        int flags = fcntl(STDIN_FILENO, F_GETFL);
        if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) { perror("control stdin"); return 1; }
    }
    struct sigaction sa = {.sa_handler = stop_signal};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (sock < 0) { perror("socket"); return 1; }
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", ip); close(sock); return 2;
    }
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); close(sock); return 1; }
    FILE *record = NULL;
    if (record_dir) {
        if (g_mkdir_with_parents(record_dir, 0755) < 0) { perror("record directory"); close(sock); return 1; }
        char stamp[32];
        time_t now = time(NULL);
        struct tm tm;
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
        /* Exclusive creation prevents accidentally overwriting a previous recording. */
        char *path = g_strdup_printf("%s/%s_%s_%s.iq", record_dir, prefix, stamp, u8 ? "u8" : "s16");
        record = fopen(path, "wbx");
        if (!record) { perror(path); g_free(path); close(sock); return 1; }
        fprintf(stderr, "[record] writing IQ to %s\n", path);
        g_free(path);
        setvbuf(record, NULL, _IONBF, 0);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    uint8_t *packet = g_malloc(bufsize), *output = g_malloc(bufsize);
    Stats input_stats = {0}, output_stats = {0};
    size_t in_measured = 0, out_measured = 0;
    size_t in_target = limit & ~(size_t)3, out_target = limit & ~(size_t)(u8 ? 1 : 3);
    int reported = 0, result = 0;
    uint64_t packets = 0, dropped = 0, input_bytes = 0, output_bytes = 0;
    fprintf(stderr, "[linrad] listening on %s:%d, header=%d, output=%s\n", ip, port, skip, u8 ? "u8" : "s16");
    IQBandFilter *filter = sample_rate > 0 ? iq_bandfilter_create(sample_rate, bandwidth) : NULL;
    if (sample_rate > 0 && !filter) {
        fprintf(stderr, "Cannot create IQ filter\n");
        if (record) fclose(record);
        g_free(packet); g_free(output); close(sock); return 1;
    }
    float *moon_iq=moon.tx ? g_new(float,bufsize) : NULL;
    capture.rate = sample_rate;
    while (!stopping) {
        if (control_stdin) {
            double unused = 0, requested_bw = bandwidth;
            int changed = tune_control_read(&control, STDIN_FILENO, 1, 0, &unused,
                1, 0, 0, &unused, sample_rate, &requested_bw);
            if ((changed & 4) && requested_bw != bandwidth) {
                if (iq_bandfilter_set(filter, requested_bw)) { fprintf(stderr, "Cannot update IQ filter\n"); result = 1; break; }
                bandwidth = requested_bw;
                fprintf(stderr, "[linrad] IQ filter width %.0f Hz (0=bypass); sample rate unchanged\n", bandwidth);
            }
        }
        iq_record_feed(&capture, NULL, 0, 0);
        struct pollfd ready = {.fd=sock, .events=POLLIN};
        int polled = poll(&ready, 1, 100);
        if (polled < 0 && errno != EINTR) { perror("poll"); result = 1; break; }
        if (polled <= 0) continue;
        ssize_t received = recv(sock, packet, bufsize, MSG_TRUNC);
        if (received < 0) {
            if (errno == EINTR) continue;
            perror("recv"); result = 1; break;
        }
        ++packets;
        if (received <= skip || received > bufsize) {
            ++dropped;
            if (verbose) fprintf(stderr, "drop packet len=%zd (short or truncated)\n", received);
            continue;
        }
        size_t n = ((size_t)received - skip) & ~(size_t)1;
        const uint8_t *payload = packet + skip;
        iq_record_feed(&capture, payload, n / 4, 0);
        size_t out_n = u8 ? n / 2 : n;
        input_bytes += n;
        if (limit && in_measured < in_target) {
            size_t chunk = MIN(n, in_target - in_measured) & ~(size_t)3;
            measure(&input_stats, payload, chunk, 0);
            in_measured += chunk;
        }
        if (!u8 && gain == 1.0) memcpy(output, payload, n);
        else for (size_t i = 0; i < n; i += 2) {
            int v = s16(payload + i);
            double value = nearbyint(u8 ? offset + (v * scale) / 256.0 : v * gain);
            if (u8) output[i / 2] = (uint8_t)fmax(0, fmin(255, value));
            else {
                uint16_t signed_value = (uint16_t)(int)fmax(-32768, fmin(32767, value));
                output[i] = signed_value & 255;
                output[i + 1] = signed_value >> 8;
            }
        }
        iq_monitor_feed(output, out_n/(u8 ? 2 : 4), u8, sample_rate);
        if (moon.tx) {
            size_t count=n/4;
            if (!moon_tracker_update(&moon,&shift,sample_rate,g_get_real_time()/1e6-count/sample_rate) ||
                !iq_shift_fits(sample_rate,shift.offset,bandwidth)) {
                fprintf(stderr,"[linrad] Moon tracking or filter outside IQ band\n");
                result=1; break;
            }
            for (size_t j=0;j<count*2;++j)
                moon_iq[j]=u8 ? ((int)output[j]-128)*256 : s16(output+2*j);
            iq_shift_process(&shift,moon_iq,count);
            iq_bandfilter_process_float(filter,moon_iq,count);
            iq_shift_pack(moon_iq,output,count,u8);
        } else if (filter) iq_bandfilter_process(filter, output, out_n/(u8 ? 2 : 4), u8);
        if (limit && !reported) {
            size_t chunk = MIN(out_n, out_target - out_measured) & ~(size_t)(u8 ? 1 : 3);
            measure(&output_stats, output, chunk, u8);
            out_measured += chunk;
            if (in_measured >= in_target && out_measured >= out_target) {
                report(&input_stats, "INPUT", 0); report(&output_stats, "OUTPUT", u8); reported = 1;
            }
        }
        if ((record && write_all(record, output, out_n)) ||
            ((!record || record_stdout) && write_all(stdout, output, out_n))) {
            perror("write IQ"); result = 1; break;
        }
        output_bytes += out_n;
        if (verbose && packets % 1000 == 0)
            fprintf(stderr, "pkts=%llu dropped=%llu in_bytes=%llu out_bytes=%llu\n",
                    (unsigned long long)packets, (unsigned long long)dropped,
                    (unsigned long long)input_bytes, (unsigned long long)output_bytes);
    }
    g_free(moon_iq);
    iq_record_stop(&capture);
    if (record && fclose(record)) { perror("close recording"); result = 1; }
    fprintf(stderr, "[stop] pkts=%llu dropped=%llu in_bytes=%llu out_bytes=%llu\n",
            (unsigned long long)packets, (unsigned long long)dropped,
            (unsigned long long)input_bytes, (unsigned long long)output_bytes);
    iq_bandfilter_destroy(filter);
    g_free(packet); g_free(output); close(sock);
    return result;
}
