#define _POSIX_C_SOURCE 200809L
#include <gtk/gtk.h>
#include "gui_theme.h"
#include "sdriq.h"
#include "funcube.h"
#include "rtlsdr.h"
#include "lime_rx.h"
#include "iq_file.h"
#include "iq_shift.h"
#include "rx_limits.h"
#include <glib-unix.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "iq_view.h"
#include "decoder_view.h"
#include "video_view.h"
#include "dvbt_backend.h"
#include "leansdr_backend.h"
#include "scatter_view.h"
#include "ts_info_view.h"
#include "program_view.h"
#include "usb_receiver.h"

enum { SOURCE_AFEDRI = 4, SOURCE_RTLSDR = 5, SOURCE_FCDPP = 6, SOURCE_LIME = 7,
       SOURCE_RSP1A = 8, SOURCE_RSP1B = 9, SOURCE_AIRSPYHF = 10, SOURCE_AIRSPYR2 = 11, SOURCE_FILE = 12 };

typedef struct { GPtrArray *args; GPid pid, group; } Stage;
typedef struct {
    GKeyFile *config;
    char *config_path, *directory;
    int standard, sr, fec, gi, exit_code, source, constellation;
    GtkWidget *source_box, *rate_box, *sdriq_rate_box, *frequency_box, *format_box;
    GtkWidget *frequency_display, *frequency_calculation, *gain_display, *offset_box, *receiver_offset_box, *step_box;
    GtkWidget *filter_box;
    GtkWidget *config_dialog, *hardware, *hardware_stack, *renderer_panel, *renderer_box;
    GtkWidget *hardware_entries[SOURCE_FILE][3], *ip_box, *port_box, *header_box, *center_box;
    GtkWidget *sr_label, *dashboard, *lower;
    GtkWidget *mode_dialog, *renderer_dialog, *mode_summary;
    GtkWidget *ldpc_mode_box, *ldpc_workers_box, *ldpc_latency_box, *s2_acquisition_box;
    GtkWidget *renderer_program, *renderer_args, *output_file, *renderer_error;
    GtkWidget *udp_enabled, *udp_host, *udp_port;
    GtkWidget *file_path, *file_format, *file_header, *file_center, *file_details;
    int renderer;
    int tune_fd;
    gboolean scatter, running, stopping, closing, headless, afedri_receiving;
    guint kill_timer, drain_timer;
    gboolean replay_eof;
    Stage stages[4];
    int stage_count;
    GtkWidget *window, *controls, *standard_box, *sr_box, *fec_box, *gi_box, *constellation_box;
    GtkWidget *scatter_box, *power_button, *power_label, *status, *preview, *log;
    GtkWidget *record_button;
    gboolean recording, record_pending;
    char record_log_line[1024];
    char receiver_offset_error[512];
    size_t record_log_used;
    GMainLoop *loop;
    IQView iq_view;
    DecoderView decoder_view;
    VideoView video_view;
    ScatterView scatter_view;
    TsInfoView information;
    ProgramView programs;
    ScatterBackend *scatter_backend;
    UsbReceiver *usb_receiver;
    GtkWidget *usb_toggle, *fm_toggle, *fm_data_toggle, *usb_port, *usb_status, *tci_dialog;
    GtkWidget *pcm_udp_enabled, *pcm_udp_host, *pcm_udp_port;
} App;

static const char *standards[] = {"DVB-S", "DVB-S2", "DVB-T"};
static const char *constellations[] = {"QPSK", "8PSK"};
static const char *s2_latency_modes[] = {"auto", "low", "throughput"};
static const char *s2_acquisition_modes[] = {"auto", "fast", "normal"};
static const char *fecs[] = {"1/2", "2/3", "3/4", "1/4", "1/3", "2/5", "3/5", "4/5", "5/6", "8/9", "9/10", "7/8"};
/* DVB-S2 MODCOD numbers, indexed by constellation and the FEC list above.
 * Zero denotes an unsupported combination. */
static const unsigned modcods[2][G_N_ELEMENTS(fecs)] = {
    {4, 6, 7, 1, 2, 3, 5, 8, 9, 10, 11},
    {0, 13, 14, 0, 0, 0, 12, 0, 15, 16, 17}
};
static gboolean valid_fec(App *a, int fec) {
    if (fec < 0 || fec >= (int)G_N_ELEMENTS(fecs)) return FALSE;
    if (a->standard == 1) return modcods[a->constellation][fec] != 0;
    if (a->standard == 2) return fec < 3 || !strcmp(fecs[fec], "5/6") || !strcmp(fecs[fec], "7/8");
    return fec < 3;
}
static const char *guards[] = {"1/32", "1/16", "1/8", "auto"};
static const char *sources[] = {"linrad", "hackrf", "b210", "sdriq", "afedri", "rtlsdr", "fcdpp", "lime", "rsp1a", "rsp1b", "airspyhf", "airspyr2", "file"};
static const char *source_labels[] = {"Linrad (NET input)", "Hackrf One", "B210 (kinex7)", "SDR-IQ (RFspace)", "Afedri Net", "RTL-SDR", "FUNcube Pro+", "LimeSDR Mini v1.2", "SDRplay RSP1A", "SDRplay RSP1B", "Airspy HF+ Dual Port (VHF)", "Airspy R2", "File (IQ)"};
static const char *file_formats[] = {"s16", "u8", "cf32"};
static gboolean hardware_source(App *a) { return a->source>0 && a->source<SOURCE_FILE; }
enum { RENDER_CVLC, RENDER_VLC, RENDER_FFPLAY, RENDER_CUSTOM, RENDER_FILE, RENDER_EMBEDDED, RENDER_UDP };
static const char *renderers[] = {"cvlc", "vlc", "ffplay", "custom", "mpegts", "embedded", "udp"};
static gboolean internal_player(App *a) { return a->renderer == RENDER_EMBEDDED || a->renderer == RENDER_UDP; }
static gboolean udp_output(App *a) {
    return a->renderer == RENDER_UDP || (a->renderer == RENDER_EMBEDDED &&
        g_key_file_get_boolean(a->config, "renderer", "udp_enabled", NULL));
}
static const char *ffplay_args = "-hide_banner -loglevel warning -autoexit -f mpegts";
static gboolean use_scatter(App *a) { return a->scatter; }
static const int airspyr2_rates[] = {2500000, 10000000};
static const int sat_rates[] = {35000, 66000, 125000, 250000, 333000, 500000, 1000000, 2000000, 4000000, 5000000};
static const int terrestrial_rates[] = {150000, 250000, 40000, 35000, 333000, 500000};

static void fatal(const char *fmt, ...) G_GNUC_PRINTF(1, 2) G_GNUC_NORETURN;
static void fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr); exit(2);
}

static char *setting(App *a, const char *section, const char *key) {
    GError *error = NULL;
    char *value = g_key_file_get_string(a->config, section, key, &error);
    if (!value) fatal("Config [%s] %s: %s", section, key, error->message);
    return value;
}

static int integer_setting(App *a, const char *section, const char *key, int low, int high) {
    char *s = setting(a, section, key), *end;
    errno = 0;
    long n = strtol(s, &end, 10);
    if (errno || end == s || *end || n < low || n > high) fatal("Config [%s] %s: invalid value %s", section, key, s);
    g_free(s); return n;
}

/* Converter LO changes RF labels; receiver offset selects a channel inside
 * the raw SDR band. Keep these independent, including raw recording metadata. */
static double receiver_offset(App *a) {
    if (!hardware_source(a) || !g_key_file_has_key(a->config,sources[a->source],"receiver_offset",NULL)) return 0;
    GError *error=NULL;
    double offset=g_key_file_get_double(a->config,sources[a->source],"receiver_offset",&error);
    if (error) { g_error_free(error); return NAN; }
    return offset;
}
static double channel_bandwidth(App *a) { return a->standard == 2 ? a->sr : a->sr*1.35; }
static const char *receiver_offset_problem(App *a) {
    if (!hardware_source(a)) return NULL;
    const char *section=sources[a->source];
    GError *error=NULL;
    double frequency=g_key_file_get_double(a->config,section,"frequency",&error);
    if (error) { g_error_free(error); frequency=NAN; error=NULL; }
    double lo=g_key_file_get_double(a->config,section,"lo_offset",&error);
    if (error) { g_error_free(error); lo=NAN; }
    double rate=g_key_file_get_double(a->config,sources[a->source],"sample_rate",NULL);
    double offset=receiver_offset(a);
    if (a->source == SOURCE_AIRSPYR2 && !rx_airspyr2_rate_valid(rate)) {
        g_snprintf(a->receiver_offset_error,sizeof(a->receiver_offset_error),
            "Airspy R2: select sample rate 2500000 or 10000000 samples/s (also checked against the connected device).");
        return a->receiver_offset_error;
    }
    if (!rx_tuning_valid(section,frequency,offset) || !isfinite(lo) ||
        !isfinite(frequency+lo+offset) || frequency+lo+offset < 0) {
        g_snprintf(a->receiver_offset_error,sizeof(a->receiver_offset_error),
            "%s: invalid SDR/IF tuning. SDR/IF %.9g Hz + converter LO %+.9g Hz + receiver offset %+.9g Hz. "
            "Hardware range: %.9g..%.9g Hz%s. Converter LO changes the RF label, not the hardware range.",
            source_labels[a->source],frequency,lo,offset,rx_frequency_min(section),rx_frequency_max(section),
            a->source == SOURCE_FCDPP ? " (240..420 MHz gap)" : "");
        return a->receiver_offset_error;
    }
    double width=channel_bandwidth(a);
    if (!rx_bandwidth_fits(section,rate,offset,width)) {
        g_snprintf(a->receiver_offset_error,sizeof(a->receiver_offset_error),
            "Receiver offset + DVB bandwidth does not fit %s. Required: %.9g Hz; usable: %.9g Hz; "
            "sample rate: %.9g samples/s; receiver offset: %+.9g Hz. "
            "%s Reduce symbol rate/bandwidth or offset, or increase sample rate.",
            source_labels[a->source],width+2*fabs(offset),rx_usable_bandwidth(section,rate),rate,offset,
            a->standard == 2 ? "DVB-T uses the selected channel bandwidth." : "DVB-S/S2 bandwidth = symbol rate x 1.35 (35% roll-off).");
        return a->receiver_offset_error;
    }
    width=g_key_file_get_double(a->config,sources[a->source],"filter_bandwidth",NULL);
    if (g_key_file_get_boolean(a->config,sources[a->source],"filter_enabled",NULL) &&
        !rx_bandwidth_fits(section,rate,offset,width)) {
        g_snprintf(a->receiver_offset_error,sizeof(a->receiver_offset_error),
            "Receiver offset + IQ filter does not fit the source band. "
            "Receiver offset: %+.9g Hz; filter bandwidth: %.9g Hz; source sample rate: %.9g samples/s. "
            "Reduce the offset or filter width.",offset,width,rate);
        return a->receiver_offset_error;
    }
    return NULL;
}

static int s2_choice(App *a, const char *key, const char *const choices[], unsigned count) {
    char *value = setting(a, "leandvb", key);
    for (unsigned i=0; i<count; ++i) if (!strcmp(value, choices[i])) { g_free(value); return i; }
    fatal("Invalid DVB-S2 %s: %s", key, value);
    return 0;
}

/* Contest rates favour reacquisition/latency; high rates favour throughput.
 * Resolve at pipeline creation so GUI changes and IQ metadata use the same rule. */
static gboolean s2_narrowband(App *a) { return a->sr <= 333000; }

static void add(Stage *s, const char *arg) { g_ptr_array_add(s->args, g_strdup(arg)); }
static void add_number(Stage *s, const char *flag, int value) {
    add(s, flag); g_ptr_array_add(s->args, g_strdup_printf("%d", value));
}

static Stage *new_stage(App *a, const char *key) {
    Stage *s = &a->stages[a->stage_count++];
    s->args = g_ptr_array_new_with_free_func(g_free);
    char *path = setting(a, "paths", key);
    if (!*path) fatal("Empty program path: %s", key);
    char *resolved = strchr(path, '/') ? g_canonicalize_filename(path, a->directory) : g_strdup(path);
    add(s, resolved); g_free(resolved); g_free(path); return s;
}

/* Parse quoting as argv; configuration text is never executed by a shell. */
static void options(App *a, Stage *s, const char *section, const char *key, const char *reserved) {
    char *value = setting(a, section, key);
    if (*value) {
        int count; char **args; GError *error = NULL;
        if (!g_shell_parse_argv(value, &count, &args, &error)) fatal("Config [%s] %s: %s", section, key, error->message);
        char **blocked = g_strsplit(reserved, " ", -1);
        for (int i = 0; i < count; ++i) {
            for (int j = 0; blocked[j]; ++j) {
                size_t n = strlen(blocked[j]);
                if (n && !strncmp(args[i], blocked[j], n) && (!args[i][n] || args[i][n] == '='))
                    fatal("Config [%s] %s: %s is controlled by the GUI/config fields", section, key, blocked[j]);
            }
            add(s, args[i]);
        }
        g_strfreev(blocked); g_strfreev(args);
    }
    g_free(value);
}

static void clear_stages(App *a) {
    for (int i = 0; i < a->stage_count; ++i) {
        g_ptr_array_unref(a->stages[i].args);
        memset(&a->stages[i], 0, sizeof(Stage));
    }
    a->stage_count = 0;
}

static void build_pipeline(App *a) {
    const char *problem=receiver_offset_problem(a);
    if (problem) fatal("%s",problem);
    if (!valid_fec(a, a->fec)) fatal("Invalid FEC for the selected standard/constellation");
    if (udp_output(a)) {
        char *host = setting(a, "renderer", "udp_host");
        struct in_addr destination;
        if (inet_pton(AF_INET, host, &destination) != 1 || destination.s_addr == INADDR_ANY ||
            destination.s_addr == INADDR_BROADCAST) fatal("Config [renderer] udp_host must be a destination IPv4 address");
        g_free(host);
        integer_setting(a, "renderer", "udp_port", 1, 65535);
    }
    clear_stages(a);
    int sample_rate = integer_setting(a, "linrad", "sample_rate", 1, 100000000);
    char *format = setting(a, "linrad", "format"), *ip = setting(a, "linrad", "ip");
    struct in_addr addr;
    if (strcmp(format, "s16") && strcmp(format, "u8")) fatal("Config [linrad] format must be s16 or u8");
    if (inet_pton(AF_INET, ip, &addr) != 1) fatal("Config [linrad] ip must be an IPv4 address");
    Stage *s;
    if (a->source == SOURCE_FILE) {
        sample_rate = integer_setting(a, "file", "sample_rate", 1, 100000000);
        g_free(format); format = setting(a, "file", "format");
        s = new_stage(a, "iq_file");
        char *path = setting(a, "file", "path"), *input_format = setting(a, "file", "input_format");
        char *resolved = *path ? g_canonicalize_filename(path, a->directory) : g_strdup("");
        add(s, "--in"); add(s, resolved);
        add_number(s, "--sample-rate", sample_rate);
        add(s, "--input-format"); add(s, input_format);
        add_number(s, "--header-bytes", integer_setting(a,"file","header_bytes",0,100000000));
        if (a->window) add(s,"--control-stdin");
        g_free(path); g_free(resolved); g_free(input_format);
    } else if (!a->source) {
    s = new_stage(a, "linrad");
    options(a, s, "linrad", "args", "--ip --port --skip --u8 --help -h --sample-rate --filter-bandwidth --control-stdin");
    add(s, "--ip"); add(s, ip);
    add_number(s, "--port", integer_setting(a, "linrad", "port", 1, 65535));
    add_number(s, "--skip", integer_setting(a, "linrad", "header_bytes", 0, 65534));
    if (!strcmp(format, "u8")) add(s, "--u8");
    add(s, "--record-stdout");
    add_number(s, "--sample-rate", sample_rate);
    if (a->window) add(s, "--control-stdin");
    } else {
        const char *section = sources[a->source];
        sample_rate = integer_setting(a, section, "sample_rate", a->source == SOURCE_AIRSPYR2 ? 2500000 : a->source == SOURCE_AIRSPYHF ? 1000 : rx_rsp(sources[a->source]) ? 62500 : a->source == SOURCE_LIME ? LIME_RX_MIN_RATE : a->source == SOURCE_FCDPP ? FUNCUBE_RATE : a->source == SOURCE_RTLSDR ? 225001 : a->source == SOURCE_AFEDRI ? 48000 : a->source == 3 ? 8138 : 2000000,
                                      a->source == SOURCE_AIRSPYR2 ? 10000000 : a->source == SOURCE_AIRSPYHF ? 768000 : rx_rsp(sources[a->source]) ? 10000000 : a->source == SOURCE_LIME ? LIME_RX_MAX_RATE : a->source == SOURCE_FCDPP ? FUNCUBE_RATE : a->source == SOURCE_RTLSDR ? 3200000 : a->source == SOURCE_AFEDRI ? 2400000 : a->source == 3 ? 196078 : a->source == 1 ? 20000000 : 61440000);
        if (a->source == SOURCE_RTLSDR && !rtlsdr_rate_valid(sample_rate)) fatal("Invalid RTL-SDR sample rate: %d", sample_rate);
        if (a->source == 3 && !sdriq_rate_valid(sample_rate)) fatal("Invalid SDR-IQ sample rate: %d", sample_rate);
        g_free(format); format = setting(a, section, "format");
        if (strcmp(format, "s16") && strcmp(format, "u8")) fatal("Invalid SDR format");
        s = new_stage(a, "sdr");
        add(s, "--source"); add(s, section);
        if (a->window) add(s, "--control-stdin");
        add(s, "--format"); add(s, format);
        add_number(s, "--sample-rate", sample_rate);
        {
            char value[G_ASCII_DTOSTR_BUF_SIZE];
            if (receiver_offset(a) != 0) {
                add(s, "--receiver-offset"); add(s,g_ascii_dtostr(value,sizeof(value),receiver_offset(a)));
            }
            add(s, "--channel-bandwidth"); add(s,g_ascii_dtostr(value,sizeof(value),channel_bandwidth(a)));
        }
        const char *keys[] = {"frequency", "gain", "args", "images", "antenna"};
        const char *flags[] = {"--frequency", "--gain", "--args", "--images", "--antenna"};
        const char *net_keys[] = {"frequency", "gain", "ip", "port", "bind"};
        const char *net_flags[] = {"--frequency", "--gain", "--ip", "--port", "--bind"};
        for (int i = 0; i < 5; ++i) {
            char *value = setting(a, section, a->source == SOURCE_AFEDRI ? net_keys[i] : keys[i]);
            add(s, a->source == SOURCE_AFEDRI ? net_flags[i] : flags[i]); add(s, value); g_free(value);
        }
    }
    const char *filter_section = sources[a->source];
    if (g_key_file_get_boolean(a->config, filter_section, "filter_enabled", NULL)) {
        int width = integer_setting(a, filter_section, "filter_bandwidth", 10, 100000000);
        if (width < sample_rate/256.0 || width > sample_rate*.95)
            fatal("IQ filter width must be between max(10, sample-rate/256) and 0.95*sample-rate");
        add_number(s, "--filter-bandwidth", width);
    }
    if (a->standard == 2) {
        s = &a->stages[a->stage_count++];
        s->args = g_ptr_array_new_with_free_func(g_free);
        char *self = g_file_read_link("/proc/self/exe", NULL);
        if (!self) fatal("Cannot locate the built-in DVB-T decoder");
        add(s, self); g_free(self); add(s, "--decoder-worker=dvbt");
        options(a, s, "rbdvbt_rx", "args", "--stdin --input-format --sample-rate --sr --gi --fec --stdout-ts --ts-out --udp-ts --udp-out --visualizer-udp");
        /* These are essential for the integrated live decode path, even when
         * an older/empty config does not supply them. */
        add(s, "--live"); add(s, "--resample-to-dvbt-rate");
        add(s, "--stdin"); add(s, "--input-format"); add(s, format);
        add_number(s, "--sample-rate", sample_rate); add_number(s, "--sr", a->sr);
        add(s, "--gi"); add(s, guards[a->gi]); add(s, "--fec"); add(s, fecs[a->fec]); add(s, "--stdout-ts");
    } else {
        char *backend=setting(a,"leandvb","backend");
        gboolean embedded=!strcmp(backend,"embedded");
        if (!embedded && strcmp(backend,"external")) fatal("Invalid LeanDVB backend: %s",backend);
        g_free(backend);
        if (embedded) {
            s=&a->stages[a->stage_count++]; s->args=g_ptr_array_new_with_free_func(g_free);
            char *self=g_file_read_link("/proc/self/exe",NULL);
            if (!self) fatal("Cannot locate the built-in LeanDVB decoder");
            add(s,self); g_free(self); add(s,"--decoder-worker=leansdr");
        } else s = new_stage(a, "leandvb");
        options(a, s, "leandvb", a->standard ? "dvbs2_args" : "dvbs_args",
                embedded ? "--s16 --u8 --f32 -f --standard --sr --cr --modcods --ldpc-helper --nhelpers --ldpc-batch" :
                           "--s16 --u8 --f32 -f --standard --sr --cr --modcods");
        if (a->standard==1) {
            int acquisition=s2_choice(a,"acquisition",s2_acquisition_modes,G_N_ELEMENTS(s2_acquisition_modes));
            if (acquisition==1 || (acquisition==0 && s2_narrowband(a))) add(s,"--fastlock");
        }
        add(s, !strcmp(format, "u8") ? "--u8" : "--s16"); add_number(s, "-f", sample_rate);
        add(s, "--standard"); add(s, standards[a->standard]); add_number(s, "--sr", a->sr);
        if (embedded && a->standard==1) {
            char *mode=setting(a,"leandvb","ldpc_mode");
            if (!strcmp(mode,"soft")) {
                add(s,"--ldpc-helper"); add(s,"builtin");
                add_number(s,"--nhelpers",integer_setting(a,"leandvb","helpers",1,8));
                int latency=s2_choice(a,"latency",s2_latency_modes,G_N_ELEMENTS(s2_latency_modes));
                add_number(s,"--ldpc-batch",latency==1 || (latency==0 && s2_narrowband(a)) ? 1 : 32);
            } else if (strcmp(mode,"bitflip")) fatal("Invalid LDPC mode: %s",mode);
            g_free(mode);
        }
        if (a->standard == 0) { add(s, "--cr"); add(s, fecs[a->fec]); }
        else {
            char *mask = g_strdup_printf("0x%x", 1u << modcods[a->constellation][a->fec]);
            add(s, "--modcods"); add(s, mask); g_free(mask);
        }
    }
    char *vlc_input = g_strdup("fd://0");
    if (a->renderer == RENDER_CUSTOM) {
        s = new_stage(a, "custom_renderer");
        options(a, s, "renderer", "args", "");
        /* The renderer chooses its own stdin syntax via {input}; no shell. */
        char *input = g_strdup("-");
        for (guint i=1; i<s->args->len; ++i) {
            char *arg = g_ptr_array_index(s->args, i);
            char **parts = g_strsplit(arg, "{input}", -1);
            g_ptr_array_index(s->args, i) = g_strjoinv(input, parts);
            g_strfreev(parts); g_free(arg);
        }
        g_free(input);
    } else if (a->renderer == RENDER_FFPLAY) {
        s = new_stage(a, "ffplay");
        options(a, s, "ffplay", "args", "-i");
        /* FFmpeg uses pipe:0 for stdin. */
        char *input = g_strdup("pipe:0");
        add(s, input); g_free(input);
    } else if (a->renderer != RENDER_FILE && !internal_player(a)) {
        s = new_stage(a, renderers[a->renderer]); options(a, s, "cvlc", "args", "");
        add(s, vlc_input);
    }
    for (int i = 0; i < a->stage_count; ++i) g_ptr_array_add(a->stages[i].args, NULL);
    g_free(vlc_input); g_free(format); g_free(ip);
}

static char *describe(App *a) {
    GString *text = g_string_new(NULL);
    for (int i = 0; i < a->stage_count; ++i) {
        if (i) g_string_append(text, "\n  |\n");
        char **args = (char **)a->stages[i].args->pdata;
        for (int j = 0; args[j]; ++j) {
            char *quoted = g_shell_quote(args[j]);
            if (j) g_string_append_c(text, ' ');
            g_string_append(text, quoted); g_free(quoted);
        }
    }
    if (internal_player(a)) {
        g_string_append(text, "\n  | ");
        g_string_append(text, a->renderer == RENDER_UDP ? "UDP output" : "embedded video panel");
        if (udp_output(a)) {
            char *host = setting(a, "renderer", "udp_host");
            g_string_append_printf(text, " + UDP MPEG-TS -> %s:%d", host,
                integer_setting(a, "renderer", "udp_port", 1, 65535));
            g_free(host);
        }
    }
    if (a->renderer == RENDER_FILE) {
        char *path = setting(a, "renderer", "output_file");
        char *resolved = g_canonicalize_filename(path, a->directory), *quoted = g_shell_quote(resolved);
        g_string_append_printf(text, "\n  > %s  (new MPEG-TS file)", quoted);
        g_free(path); g_free(resolved); g_free(quoted);
    }
    if (use_scatter(a)) g_string_append(text, "\n  + decoder TS -> embedded TSscatterFix (parallel) -> RGB image / service / progress panel");
    return g_string_free(text, FALSE);
}

static void status(App *a, const char *message) {
    fprintf(stderr, "[pipeline] %s\n", message);
    if (a->status) {
        gtk_label_set_text(GTK_LABEL(a->status), message);
        gtk_widget_set_visible(a->status, *message != '\0');
    }
}

static void log_text(App *a, const char *text) {
    if (!a->log) return;
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->log));
    GtkTextIter end, start;
    gtk_text_buffer_get_end_iter(b, &end);
    char *valid = g_utf8_make_valid(text, -1);
    gtk_text_buffer_insert(b, &end, valid, -1); g_free(valid);
    int chars = gtk_text_buffer_get_char_count(b);
    if (chars > 100000) {
        gtk_text_buffer_get_start_iter(b, &start);
        gtk_text_buffer_get_iter_at_offset(b, &end, chars - 80000);
        gtk_text_buffer_delete(b, &start, &end);
    }
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(a->log), &end, 0, FALSE, 0, 0);
}

static void record_toggled(GtkToggleButton *button, gpointer data);
static void power_refresh(App *a);

static void record_refresh(App *a) {
    if (!a->record_button) return;
    g_signal_handlers_block_by_func(a->record_button, record_toggled, a);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->record_button), a->recording);
    g_signal_handlers_unblock_by_func(a->record_button, record_toggled, a);
    gtk_button_set_label(GTK_BUTTON(a->record_button), a->record_pending ? "IQ · waiting…" :
        a->recording ? "● Stop IQ" : "● Record IQ");
    gtk_widget_set_sensitive(a->record_button,
        a->source != SOURCE_FILE && (a->source != SOURCE_AFEDRI || a->afedri_receiving) && a->running && !a->stopping && !a->record_pending && a->tune_fd >= 0);
    gtk_widget_set_tooltip_text(a->record_button, a->source == SOURCE_FILE ?
        "IQ recording is unavailable during file playback." : !a->running ?
        "Start SDR to record full-band s16 IQ." : "Toggle full-band s16 IQ recording in the configuration directory.");
}

static void record_toggled(GtkToggleButton *button, gpointer data) {
    App *a = data;
    if (a->source == SOURCE_FILE || !a->running || a->stopping || a->record_pending || a->tune_fd < 0) {
        record_refresh(a); return;
    }
    char *command;
    if (gtk_toggle_button_get_active(button)) {
        const char *section = sources[a->source];
        double frequency = g_key_file_get_double(a->config, section, "frequency", NULL);
        if (hardware_source(a)) frequency += g_key_file_get_double(a->config, section, "lo_offset", NULL);
        int rate = integer_setting(a, section, "sample_rate", 1, 100000000);
        GDateTime *now = g_date_time_new_now_utc();
        char *stamp = g_date_time_format(now, "%Y%m%dT%H%M%S.%fZ");
        command = g_strdup_printf("record %.0f linrad-dvb-%s-%.0f-%d.iq\n", frequency, stamp, frequency, rate);
        g_free(stamp); g_date_time_unref(now);
    } else command = g_strdup("record-stop\n");
    ssize_t sent;
    do { sent = write(a->tune_fd, command, strlen(command)); } while (sent < 0 && errno == EINTR);
    if (sent == (ssize_t)strlen(command)) a->record_pending = TRUE;
    else status(a, "Cannot send IQ recording command; try again.");
    g_free(command); record_refresh(a);
}

/* stderr shares a pipe with decoder logs; retain partial lines across reads. */
static void record_log(App *a, const char *text, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (text[i] == '\n') {
            a->record_log_line[a->record_log_used] = 0;
            const char *line = a->record_log_line;
            if (g_str_has_prefix(line, "[iq-record] ")) {
                a->recording = a->running && !a->stopping && g_str_has_prefix(line, "[iq-record] started ");
                a->record_pending = FALSE;
                status(a, line + strlen("[iq-record] "));
                record_refresh(a);
            }
            if (a->source == SOURCE_AFEDRI && a->running && !a->stopping &&
                g_str_has_prefix(line, "[AFEDRI] ")) {
                if (g_str_has_prefix(line, "[AFEDRI] Receiving IQ:")) {
                    a->afedri_receiving = TRUE;
                    power_refresh(a);
                }
                status(a, line + strlen("[AFEDRI] "));
            }
            a->record_log_used = 0;
        } else if (a->record_log_used < sizeof(a->record_log_line)-1)
            a->record_log_line[a->record_log_used++] = text[i];
    }
}

static gboolean read_log(gint fd, GIOCondition condition, gpointer data) {
    App *a = data;
    char buf[4097]; ssize_t n;
    /* Bound each callback so continuous decoder logging cannot starve Stop. */
    for (int i = 0; i < 16; ++i) {
        n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) { buf[n] = 0; log_text(a, buf); record_log(a, buf, n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (!n || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { close(fd); return G_SOURCE_REMOVE; }
        break;
    }
    if (condition & G_IO_NVAL) { close(fd); return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

static void scatter_refresh(App *a) {
    if (a->scatter_view.frame) gtk_widget_set_visible(a->scatter_view.frame, use_scatter(a));
    if (a->programs.frame) gtk_widget_set_visible(a->programs.frame, !use_scatter(a));
    if (!a->scatter_box) return;
    gtk_widget_set_sensitive(a->scatter_box, !a->stopping);
    gtk_widget_set_tooltip_text(a->scatter_box, a->stopping ? "Waiting for reception to stop." :
        a->scatter ? "TSscatterFix on · click to turn off." : "TSscatterFix off · click to turn on.");
}

static void power_refresh(App *a) {
    scatter_refresh(a);
    record_refresh(a);
    if (!a->power_button) return;
    GtkStyleContext *style = gtk_widget_get_style_context(a->power_button);
    const char *states[] = {"receiver-off", "receiver-running", "receiver-stopping", "receiver-error"};
    for (unsigned i=0; i<G_N_ELEMENTS(states); ++i) gtk_style_context_remove_class(style, states[i]);
    int state = a->stopping ? 2 : a->running ? 1 : a->exit_code ? 3 : 0;
    gboolean connecting = a->source == SOURCE_AFEDRI && a->running && !a->stopping && !a->afedri_receiving;
    gtk_style_context_add_class(style, states[connecting ? 2 : state]);
    const char *labels[] = {"Start SDR", "Stop SDR · running", "Stopping SDR…", "Start SDR · error"};
    const char *file_labels[] = {"Play file", "Stop file · playing", "Stopping playback…", "Play file · error"};
    const char *tips[] = {"Receiver off. Click to start.",
        "Receiver pipeline running. Click to stop.",
        "Stopping the receiver pipeline. Please wait…",
        "Reception stopped due to an error. Check the status and log; click to restart."};
    gtk_label_set_text(GTK_LABEL(a->power_label), connecting ? "Stop SDR · connecting Afedri…" : a->source==SOURCE_FILE ? file_labels[state] : labels[state]);
    const char *problem=a->running ? NULL : receiver_offset_problem(a);
    gtk_widget_set_tooltip_text(a->power_button, problem ? problem : connecting ? "Waiting for confirmed Afedri IQ reception. Click to stop." : tips[state]);
    gtk_widget_set_sensitive(a->power_button, !a->stopping && !problem);
}

static void finish_if_stopped(App *a) {
    if (!a->stopping || a->kill_timer) return;
    for (int i = 0; i < a->stage_count; ++i) if (a->stages[i].pid) return;
    if (!scatter_backend_done(a->scatter_backend)) return;
    scatter_backend_destroy(a->scatter_backend); a->scatter_backend = NULL;
    if (a->video_view.area) video_view_stop(&a->video_view);
    if (a->tune_fd >= 0) { close(a->tune_fd); a->tune_fd = -1; }
    a->running = a->stopping = FALSE;
    a->recording = a->record_pending = FALSE;
    if (!a->exit_code) status(a, a->replay_eof ? "IQ file playback complete" : "Stopped");
    if (a->controls) {
        gtk_widget_set_sensitive(a->controls, TRUE);
        gtk_widget_set_sensitive(a->hardware, TRUE);
        gtk_widget_set_sensitive(a->renderer_panel, TRUE);
        gtk_widget_set_sensitive(a->frequency_box, hardware_source(a));
    }
    power_refresh(a);
    if (a->headless) g_main_loop_quit(a->loop);
    else if (a->closing) gtk_main_quit();
}

static gboolean force_stop(gpointer data) {
    App *a = data;
    for (int i = 0; i < a->stage_count; ++i) {
        if (a->stages[i].group > 0) kill(-a->stages[i].group, SIGKILL);
        a->stages[i].group = 0;
    }
    a->kill_timer = 0;
    if (!scatter_backend_done(a->scatter_backend)) {
        a->kill_timer = g_timeout_add(50, force_stop, a);
        return G_SOURCE_REMOVE;
    }
    finish_if_stopped(a); return G_SOURCE_REMOVE;
}

static void stop_pipeline(App *a) {
    if (!a->running || a->stopping) return;
    a->stopping = TRUE;
    usb_receiver_source(a->usb_receiver,a->iq_view.center,a->iq_view.rate,FALSE);
    ts_info_view_reset(&a->information);
    program_view_update(&a->programs, &(TsInfoSnapshot){0}, 0, FALSE);
    scatter_backend_stop(a->scatter_backend);
    if (a->video_view.area) video_view_stop(&a->video_view);
    scatter_view_reset(&a->scatter_view);
    decoder_view_stop(&a->decoder_view);
    decoder_view_tick(&a->decoder_view, FALSE, standards[a->standard]);
    if (a->drain_timer) { g_source_remove(a->drain_timer); a->drain_timer=0; }
    if (a->tune_fd >= 0) { close(a->tune_fd); a->tune_fd = -1; }
    if (!a->exit_code) status(a, "Stopping pipeline...");
    power_refresh(a);
    for (int i = 0; i < a->stage_count; ++i)
        if (a->stages[i].group > 0) kill(-a->stages[i].group, SIGTERM);
    a->kill_timer = g_timeout_add(1500, force_stop, a);
}

static gboolean replay_drained(gpointer data) {
    App *a=data; a->drain_timer=0;
    if (a->stages[1].pid) {
        a->exit_code=1; status(a,"Decoder did not stop after the end of the IQ file");
    }
    stop_pipeline(a); return G_SOURCE_REMOVE;
}

static void child_exit(GPid pid, gint result, gpointer data) {
    App *a = data;
    for (int i = 0; i < a->stage_count; ++i) if (a->stages[i].pid == pid) {
        a->stages[i].pid = 0;
        if (!a->stopping && a->source==SOURCE_FILE && WIFEXITED(result) && WEXITSTATUS(result)==0) {
            /* Let the decoder drain its input before terminating a player that
             * keeps its window open at EOF. Source/decoder callbacks can arrive
             * in either order. A nonzero exit is still an error. */
            if (i<2 && !a->replay_eof) {
                a->replay_eof=TRUE;
                status(a,"End of IQ file; finishing decoder and player…");
            }
            if (a->tune_fd>=0 && !a->stages[0].pid) { close(a->tune_fd); a->tune_fd=-1; }
            if (a->replay_eof) {
                if (a->drain_timer) g_source_remove(a->drain_timer);
                a->drain_timer=g_timeout_add(a->stages[1].pid ? 10000 : 1500,replay_drained,a);
                gboolean alive=FALSE;
                for (int j=0; j<a->stage_count; ++j) if (a->stages[j].pid) alive=TRUE;
                if (!alive && !internal_player(a)) stop_pipeline(a);
            } else stop_pipeline(a);
            break;
        }
        if (!a->stopping) {
            char *message = g_strdup_printf("%s stopped (%s %d); stopping the entire pipeline.",
                (char *)a->stages[i].args->pdata[0], WIFEXITED(result) ? "exit code" : "signal",
                WIFEXITED(result) ? WEXITSTATUS(result) : WTERMSIG(result));
            a->exit_code = 1; status(a, message); g_free(message); stop_pipeline(a);
        }
        break;
    }
    g_spawn_close_pid(pid); finish_if_stopped(a);
}

static void child_setup(gpointer unused) {
    (void)unused;
    /* Each process group includes decoder/player descendants. */
    if (setsid() < 0) _exit(126);
    signal(SIGPIPE, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
}

static void start_pipeline(App *a) {
    if (a->running) return;
    const char *problem=receiver_offset_problem(a);
    if (problem) { a->exit_code=1; status(a,problem); return; }
    a->record_log_used = 0;
    a->afedri_receiving = FALSE;
    build_pipeline(a); a->exit_code = 0;
    if (internal_player(a) && !a->video_view.area) {
        a->exit_code = 1;
        status(a, "Embedded/UDP playback requires the GUI; use --renderer cvlc, ffplay or mpegts with --run.");
        return;
    }
    a->replay_eof=FALSE;
    if (a->source==SOURCE_FILE) {
        char *path=setting(a,"file","path"), *fmt=setting(a,"file","input_format");
        char *resolved=g_canonicalize_filename(path,a->directory);
        GError *error=NULL; guint64 samples;
        gboolean valid=*path && iq_file_validate(resolved,fmt,integer_setting(a,"file","header_bytes",0,100000000),&samples,&error);
        g_free(path); g_free(fmt); g_free(resolved);
        if (!valid) { a->exit_code=1; status(a,error ? error->message : "Select an IQ file in Input first"); g_clear_error(&error); return; }
    }
    a->iq_view.rows = 0; a->iq_view.last_frame = 0;
    for (int i = 0; i < a->stage_count; ++i) {
        char *program = a->stages[i].args->pdata[0], *found = g_find_program_in_path(program);
        if (!found) {
            char *message = g_strdup_printf("Program not found or not executable: %s", program);
            status(a, message); g_free(message); a->exit_code = 1; return;
        }
        g_free(found);
    }
    int pipes[2][2], scatter_pipe[2] = {-1, -1}, log_pipe[2] = {-1, -1}, tune_pipe[2] = {-1, -1}, made = 0, nullfd = -1, output_fd = -1;
    if (a->renderer == RENDER_FILE) {
        char *path = setting(a, "renderer", "output_file");
        char *resolved = g_canonicalize_filename(path, a->directory);
        output_fd = open(resolved, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (output_fd < 0) {
            char *message = g_strdup_printf("Cannot create MPEG-TS file: %s: %s. Choose a new filename.", resolved, g_strerror(errno));
            status(a, message); g_free(message); g_free(path); g_free(resolved); a->exit_code = 1; return;
        }
        g_free(path); g_free(resolved);
    }
    a->tune_fd = -1;
    if (a->window) {
        if (!g_unix_open_pipe(tune_pipe, FD_CLOEXEC, NULL)) goto pipe_error;
        if (!g_unix_set_fd_nonblocking(tune_pipe[1], TRUE, NULL)) goto pipe_error;
    }
    for (; made < (a->renderer == RENDER_FILE ? 1 : 2); ++made)
        if (!g_unix_open_pipe(pipes[made], FD_CLOEXEC, NULL)) goto pipe_error;
    if (!g_unix_open_pipe(scatter_pipe, FD_CLOEXEC, NULL)) goto pipe_error;
    nullfd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (nullfd < 0) goto pipe_error;
    if (!a->headless) {
        if (!g_unix_open_pipe(log_pipe, FD_CLOEXEC, NULL)) goto pipe_error;
        if (!g_unix_set_fd_nonblocking(log_pipe[0], TRUE, NULL)) goto pipe_error;
    }
    if (internal_player(a)) {
        char *url = NULL, *destination = NULL;
        if (udp_output(a)) {
            char *host = setting(a, "renderer", "udp_host");
            destination = g_strdup_printf("%s:%d", host, integer_setting(a, "renderer", "udp_port", 1, 65535));
            g_free(host);
        }
        GError *error = NULL;
        gboolean started = video_view_start(&a->video_view, pipes[1][0], url, destination,
                                             a->renderer == RENDER_EMBEDDED, &error);
        g_free(url); g_free(destination);
        if (!started) {
            status(a, error->message); g_clear_error(&error);
            a->exit_code = 1; goto pipe_error;
        }
    }
    {
        /* Keep the TS relay available so recovery can be toggled live. */
        GError *error = NULL;
        scatter_view_reset(&a->scatter_view);
        a->scatter_backend = scatter_backend_start(scatter_pipe[0],
            output_fd >= 0 ? output_fd : pipes[1][1], a->scatter, &error);
        if (!a->scatter_backend) {
            status(a, error->message); g_clear_error(&error);
            a->exit_code = 1; goto pipe_error;
        }
    }
    if (a->window && !decoder_view_start(&a->decoder_view))
        log_text(a, "Decoder telemetry unavailable; reception continues.\n");
    a->running = TRUE;
    if (a->controls) {
        gtk_widget_set_sensitive(a->controls, FALSE);
        gtk_widget_set_sensitive(a->hardware, FALSE);
        gtk_widget_set_sensitive(a->renderer_panel, FALSE);
        gtk_widget_set_sensitive(a->frequency_box, FALSE);
    }
    /* Launch consumers first. GLib closes unused pipe descriptors in children. */
    for (int i = a->stage_count - 1; i >= 0; --i) {
        int in = i == 0 ? nullfd : pipes[i - 1][0];
        if (i == 0 && tune_pipe[0] >= 0) in = tune_pipe[0];
        int out = i == 1 && output_fd >= 0 ? output_fd : i < 2 ? pipes[i][1] : nullfd;
        if (i == 1) out = scatter_pipe[1];
        GError *error = NULL; Stage *s = &a->stages[i];
        char **env = g_get_environ();
        env = g_environ_unsetenv(env, "LINRAD_IQ_MONITOR_PORT");
        env = g_environ_unsetenv(env, "LINRAD_DECODER_MONITOR_PORT");
        env = g_environ_unsetenv(env, "LINRAD_USB_TAP");
        if (i == 0 && a->usb_receiver)
            env = g_environ_setenv(env,"LINRAD_USB_TAP",usb_receiver_tap_path(a->usb_receiver),TRUE);
        if (i == 1 && a->decoder_view.port) {
            char port[16]; g_snprintf(port, sizeof(port), "%d", a->decoder_view.port);
            env = g_environ_setenv(env, "LINRAD_DECODER_MONITOR_PORT", port, TRUE);
        }
        if (i == 0 && a->window && a->iq_view.port) {
            char port[16]; g_snprintf(port, sizeof(port), "%d", a->iq_view.port);
            env = g_environ_setenv(env, "LINRAD_IQ_MONITOR_PORT", port, TRUE);
        }
        gboolean spawned = g_spawn_async_with_fds(a->directory, (char **)s->args->pdata, env,
                G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, child_setup, NULL,
                &s->pid, in, out, a->headless ? STDERR_FILENO : log_pipe[1], &error);
        g_strfreev(env);
        if (!spawned) {
            a->exit_code = 1; status(a, error->message); g_clear_error(&error); stop_pipeline(a); break;
        }
        s->group = s->pid; g_child_watch_add(s->pid, child_exit, a);
    }
    for (int i = 0; i < made; ++i) { close(pipes[i][0]); close(pipes[i][1]); }
    close(nullfd);
    if (scatter_pipe[0] >= 0) { close(scatter_pipe[0]); close(scatter_pipe[1]); }
    if (output_fd >= 0) close(output_fd);
    if (tune_pipe[0] >= 0) close(tune_pipe[0]);
    if (a->stopping) { if (tune_pipe[1] >= 0) close(tune_pipe[1]); }
    else a->tune_fd = tune_pipe[1];
    if (log_pipe[1] >= 0) close(log_pipe[1]);
    if (log_pipe[0] >= 0) g_unix_fd_add(log_pipe[0], G_IO_IN | G_IO_HUP | G_IO_ERR, read_log, a);
    if (!a->stopping) status(a, a->source==SOURCE_FILE ? "Playing IQ file in real time" : "Pipeline running - waiting for/receiving IQ");
    return;
pipe_error:
    if (scatter_pipe[0] >= 0) { close(scatter_pipe[0]); close(scatter_pipe[1]); }
    if (a->video_view.area) video_view_stop(&a->video_view);
    if (output_fd >= 0) close(output_fd);
    if (tune_pipe[0] >= 0) close(tune_pipe[0]);
    if (tune_pipe[1] >= 0) close(tune_pipe[1]);
    for (int i = 0; i < made; ++i) { close(pipes[i][0]); close(pipes[i][1]); }
    if (nullfd >= 0) close(nullfd);
    if (log_pipe[0] >= 0) { close(log_pipe[0]); close(log_pipe[1]); }
    if (!a->exit_code) status(a, "Cannot open pipeline pipes");
    a->exit_code = 1;
}

static void refresh_summaries(App *a) {
    if (!a->mode_summary) return;
    char *text = g_strdup_printf("%s · %s %d%s · FEC %s",
        standards[a->standard], a->standard == 2 ? "BW" : "Symb/s", a->sr,
        a->standard == 2 ? " Hz" : "", fecs[a->fec]);
    gtk_label_set_text(GTK_LABEL(a->mode_summary), text); g_free(text);
    scatter_refresh(a);
}

static void refresh_preview(App *a) {
    const char *problem=receiver_offset_problem(a);
    power_refresh(a);
    if (a->sr_box) gtk_widget_queue_draw(a->sr_box);
    if (problem) {
        clear_stages(a);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->preview)),problem,-1);
        status(a,problem); refresh_summaries(a); return;
    }
    build_pipeline(a); char *text = describe(a);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->preview)), text, -1); g_free(text);
    refresh_summaries(a);
}

static void tuning_changed(GtkWidget *widget, gpointer data);

static void tuning_refresh(App *a) {
    if (!a->frequency_display) return;
    gtk_widget_set_sensitive(a->gain_display, hardware_source(a));
    if (hardware_source(a)) {
        char *gain=g_strdup_printf(a->source == SOURCE_AIRSPYR2 ? "Gain setting %.0f" : a->source == SOURCE_AIRSPYHF ? "Gain: automatic (VHF)" : rx_rsp(sources[a->source]) ? "IF reduction %.0f dB" : a->source == SOURCE_FCDPP ? "IF gain %.0f dB" : a->source == SOURCE_RTLSDR ? "Gain %.1f dB" : "Gain %.0f dB",g_key_file_get_double(a->config,sources[a->source],"gain",NULL));
        gtk_label_set_text(GTK_LABEL(a->gain_display),gain); g_free(gain);
    } else gtk_label_set_text(GTK_LABEL(a->gain_display),"Gain —");
    gtk_widget_set_sensitive(a->offset_box, hardware_source(a));
    gtk_widget_set_sensitive(a->receiver_offset_box, hardware_source(a));
    gtk_widget_set_sensitive(a->step_box, hardware_source(a));
    if (a->source==SOURCE_FILE) {
        double frequency=g_key_file_get_double(a->config,"file","frequency",NULL);
        char *text=g_strdup_printf("%.6f MHz",frequency/1e6);
        gtk_label_set_text(GTK_LABEL(a->frequency_display),text); g_free(text);
        gtk_label_set_text(GTK_LABEL(a->frequency_calculation),"File input: the receive frequency only sets the display scale; hardware tuning and gain are disabled.");
        return;
    }
    if (!a->source) {
        gtk_label_set_text(GTK_LABEL(a->frequency_display), "Tune in Linrad");
        gtk_label_set_text(GTK_LABEL(a->frequency_calculation), "Set the hardware frequency and any LO offset in Linrad.");
        return;
    }
    const char *section = sources[a->source];
    double offset = g_key_file_get_double(a->config, section, "lo_offset", NULL);
    double rx_offset=receiver_offset(a);
    a->iq_view.receiver_offset=rx_offset;
    double step = g_key_file_get_double(a->config, section, "tune_step", NULL);
    double frequency = g_key_file_get_double(a->config, section, "frequency", NULL);
    g_signal_handlers_block_by_func(a->receiver_offset_box, tuning_changed, a);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->receiver_offset_box),isfinite(rx_offset) ? rx_offset/1000.0 : 0);
    g_signal_handlers_unblock_by_func(a->receiver_offset_box, tuning_changed, a);
    g_signal_handlers_block_by_func(a->offset_box, tuning_changed, a);
    g_signal_handlers_block_by_func(a->step_box, tuning_changed, a);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->offset_box), offset/1000.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->step_box), step);
    g_signal_handlers_unblock_by_func(a->offset_box, tuning_changed, a);
    g_signal_handlers_unblock_by_func(a->step_box, tuning_changed, a);
    char *text = g_strdup_printf("%.6f MHz", (frequency + offset + rx_offset)/1e6);
    gtk_label_set_text(GTK_LABEL(a->frequency_display), text); g_free(text);
    text = g_strdup_printf("SDR/IF %.6f MHz + converter LO %+.6f MHz + receiver offset %+.3f kHz\n"
        "Receive %.6f MHz · spectrum center %.6f MHz\n%s",
        frequency/1e6,offset/1e6,rx_offset/1e3,(frequency+offset+rx_offset)/1e6,(frequency+offset)/1e6,
        receiver_offset_problem(a) ? receiver_offset_problem(a) : rx_offset == 0 ?
        "Receiver offset 0: digital frequency shift bypassed." : "DVB channel shifted digitally; spectrum and IQ recording retain the full SDR band.");
    gtk_label_set_text(GTK_LABEL(a->frequency_calculation), text); g_free(text);
}

static void tuning_changed(GtkWidget *widget, gpointer data) {
    App *a = data;
    if (!hardware_source(a)) return;
    if (widget == a->receiver_offset_box && a->running) return;
    const char *section = sources[a->source];
    g_key_file_set_double(a->config,section,"receiver_offset",
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->receiver_offset_box))*1000.0);
    g_key_file_set_double(a->config, section, "lo_offset",
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->offset_box))*1000.0);
    g_key_file_set_double(a->config, section, "tune_step",
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->step_box)));
    tuning_refresh(a);
    if (!a->running) refresh_preview(a);
    if (a->iq_view.area) gtk_widget_queue_draw(a->iq_view.area);
}

static void source_changed(GtkWidget *widget, gpointer data);
static void file_controls_refresh(App *a);

static void tune_step(App *a, int direction) {
    if (!hardware_source(a) || a->stopping) return;
    const char *section = sources[a->source];
    double frequency = g_key_file_get_double(a->config, section, "frequency", NULL);
    double step = g_key_file_get_double(a->config, section, "tune_step", NULL);
    double next = frequency + direction*step;
    if (!rx_tuning_valid(section,next,receiver_offset(a))) {
        status(a, "SDR/IF tuning limit reached (converter LO does not extend the hardware range)"); return;
    }
    if (a->running) {
        char command[64];
        int length = g_snprintf(command, sizeof(command), "%.0f\n", next);
        ssize_t sent;
        do { sent = a->tune_fd < 0 ? -1 : write(a->tune_fd, command, (size_t)length); }
        while (sent < 0 && a->tune_fd >= 0 && errno == EINTR);
        if (sent != length) { status(a, "Tuning command not sent; try again"); return; }
    }
    if (a->scatter_backend) ts_info_backend_reset(a->scatter_backend->information);
    ts_info_view_reset(&a->information);
    program_view_update(&a->programs, &(TsInfoSnapshot){0}, 0, FALSE);
    g_key_file_set_double(a->config, section, "frequency", next);
    g_signal_handlers_block_by_func(a->frequency_box, source_changed, a);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->frequency_box), next/1000.0);
    g_signal_handlers_unblock_by_func(a->frequency_box, source_changed, a);
    tuning_refresh(a);
    /* Never rebuild Stage objects while their child processes are alive. */
    if (!a->running) refresh_preview(a);
}

static void gain_step(App *a, int direction) {
    if (a->source == SOURCE_AIRSPYHF) { status(a,"Airspy HF+ VHF uses automatic gain control"); return; }
    if (!hardware_source(a) || a->stopping) return;
    const char *section = sources[a->source];
    double gain = g_key_file_get_double(a->config, section, "gain", NULL);
    double next = gain + direction*(a->source == SOURCE_AFEDRI ? 3 : a->source == 3 ? 10 : 1);
    double low = rx_rsp(section) ? 20 : a->source == SOURCE_AFEDRI ? -10 : a->source == 3 ? -20 : 0;
    double high = a->source == SOURCE_AIRSPYR2 ? 45 : rx_rsp(section) ? 59 : a->source == SOURCE_LIME ? LIME_RX_MAX_GAIN : a->source == SOURCE_FCDPP ? FUNCUBE_MAX_GAIN : a->source == SOURCE_RTLSDR ? RTLSDR_MAX_GAIN : a->source == SOURCE_AFEDRI ? 35 : a->source == 3 ? 10 : a->source == 1 ? 116 : 76;
    if (a->source == SOURCE_RTLSDR) next = CLAMP(next, low, high);
    if (next == gain || next < low || next > high) { status(a, "SDR gain limit reached"); return; }
    if (a->running) {
        /* The helper expects a decimal point, regardless of GTK's locale. */
        char command[64], value[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(value, sizeof(value), "%.1f", next);
        int length = g_snprintf(command, sizeof(command), "gain %s\n", value);
        ssize_t sent;
        do { sent = a->tune_fd < 0 ? -1 : write(a->tune_fd, command, (size_t)length); }
        while (sent < 0 && a->tune_fd >= 0 && errno == EINTR);
        if (sent != length) { status(a, "Gain command not sent; try again"); return; }
    }
    g_key_file_set_double(a->config, section, "gain", next);
    tuning_refresh(a);
    if (!a->running) refresh_preview(a);
}

static void filter_changed(GtkWidget *widget, gpointer data);
static void filter_refresh(App *a) {
    if (!a->filter_box) return;
    const char *section = sources[a->source];
    int rate = integer_setting(a, section, "sample_rate", 1, 100000000);
    int low = MAX(10, (rate+255)/256), high = MAX(low, (int)(rate*.95));
    int width = g_key_file_get_integer(a->config, section, "filter_bandwidth", NULL);
    width = CLAMP(width, low, high);
    g_key_file_set_integer(a->config, section, "filter_bandwidth", width);
    g_signal_handlers_block_by_func(a->filter_box, filter_changed, a);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->filter_box),
        g_key_file_get_boolean(a->config, section, "filter_enabled", NULL));
    gtk_widget_set_sensitive(a->filter_box, rate >= 11);
    g_signal_handlers_unblock_by_func(a->filter_box, filter_changed, a);
    a->iq_view.bandwidth = width;
    a->iq_view.enabled = g_key_file_get_boolean(a->config, section, "filter_enabled", NULL);
    a->iq_view.receiver_offset = receiver_offset(a);
    gtk_widget_set_tooltip_text(a->filter_box, a->iq_view.enabled ?
        "IQ bandpass filter on. Click to turn off. [ and ] adjust the bandwidth." :
        "IQ bandpass filter off. Click to turn on. [ and ] adjust the bandwidth.");
    if (a->iq_view.area) gtk_widget_queue_draw(a->iq_view.area);
}
static void filter_apply(App *a, int width, gboolean enabled) {
    if (a->stopping) { filter_refresh(a); return; }
    const char *section = sources[a->source];
    if (enabled && !rx_bandwidth_fits(section,g_key_file_get_double(a->config,section,"sample_rate",NULL),receiver_offset(a),width)) {
        status(a,"Filter width + receiver offset exceeds source band"); filter_refresh(a); return;
    }
    if (a->running) {
        char command[64]; int length = g_snprintf(command, sizeof(command), "bandwidth %d\n", enabled ? width : 0);
        ssize_t sent;
        do { sent = a->tune_fd < 0 ? -1 : write(a->tune_fd, command, (size_t)length); }
        while (sent < 0 && a->tune_fd >= 0 && errno == EINTR);
        if (sent != length) { status(a, "Filter command not sent"); filter_refresh(a); return; }
    }
    g_key_file_set_boolean(a->config, section, "filter_enabled", enabled);
    g_key_file_set_integer(a->config, section, "filter_bandwidth", width);
    filter_refresh(a);
    if (!a->running) refresh_preview(a);
}
static void filter_changed(GtkWidget *widget, gpointer data) {
    App *a = data; (void)widget;
    int width = g_key_file_get_integer(a->config, sources[a->source], "filter_bandwidth", NULL);
    filter_apply(a, width, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->filter_box)));
}
static void filter_step(App *a, int direction) {
    if (a->stopping || !gtk_widget_is_sensitive(a->filter_box)) return;
    const char *section = sources[a->source];
    int rate = integer_setting(a, section, "sample_rate", 1, 100000000);
    int low = MAX(10, (rate+255)/256), high = MAX(low, (int)(rate*.95));
    int width = g_key_file_get_integer(a->config, section, "filter_bandwidth", NULL);
    int step = MAX(1, width/20); /* 5% of the current total width. */
    filter_apply(a, CLAMP(width + direction*step, low, high), TRUE);
}

static void save_settings(App *a) {
    /* Commit any numeric text still being edited before serializing. */
    if (a->window) {
        GtkWidget *fields[] = {a->offset_box, a->receiver_offset_box, a->step_box, a->frequency_box,
                              a->rate_box};
        for (unsigned i = 0; i < G_N_ELEMENTS(fields); ++i)
            if (gtk_widget_is_sensitive(fields[i])) gtk_spin_button_update(GTK_SPIN_BUTTON(fields[i]));
        g_key_file_set_double(a->config, "display", "floor_dbfs_hz", a->iq_view.floor);
        g_key_file_set_double(a->config, "display", "range_db", a->iq_view.span);
    }
    g_key_file_set_string(a->config, "session", "source", sources[a->source]);
    g_key_file_set_string(a->config, "session", "standard", standards[a->standard]);
    g_key_file_set_integer(a->config, "session", "sr", a->sr);
    g_key_file_set_string(a->config, "session", "fec", fecs[a->fec]);
    g_key_file_set_string(a->config, "session", "gi", guards[a->gi]);
    g_key_file_set_string(a->config, "session", "constellation", constellations[a->standard == 1 ? a->constellation : 0]);
    g_key_file_set_boolean(a->config, "session", "scatterfix", a->scatter);
    g_key_file_set_string(a->config, "session", "renderer", renderers[a->renderer]);
    GError *error = NULL;
    gsize length;
    char *contents = g_key_file_to_data(a->config, &length, &error);
    gboolean saved = contents && g_file_set_contents_full(a->config_path, contents, length,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, &error);
    char *message = saved ? g_strdup_printf("Settings saved: %s", a->config_path) :
        g_strdup_printf("Save failed: %s", error ? error->message : "unknown error");
    status(a, message);
    g_free(message); g_free(contents); g_clear_error(&error);
}

static void save_screenshot(App *a) {
    GDateTime *now = g_date_time_new_now_utc();
    char *timestamp = g_date_time_format(now, "%Y%m%dT%H%M%S.%fZ");
    const char *section = sources[a->source];
    double frequency = g_key_file_get_double(a->config, section, "frequency", NULL);
    if (hardware_source(a))
        frequency += g_key_file_get_double(a->config, section, "lo_offset", NULL);
    frequency += receiver_offset(a);
    char khz[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(khz, sizeof(khz), "%.0f", frequency / 1e3);
    char *name = g_strdup_printf("linrad-dvb-%s-%s-%s%d-%skHz.png", timestamp,
        standards[a->standard], a->standard == 2 ? "BW" : "SR", a->sr, khz);
    char *path = g_build_filename(a->directory, name, NULL);

    /* Paint the whole widget tree, including our Cairo-backed video frame.
     * This also works on Wayland and when another window covers the receiver. */
    int scale = gtk_widget_get_scale_factor(a->window);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
        gtk_widget_get_allocated_width(a->window) * scale,
        gtk_widget_get_allocated_height(a->window) * scale);
    cairo_surface_set_device_scale(surface, scale, scale);
    cairo_t *cr = cairo_create(surface);
    gtk_widget_draw(a->window, cr);
    cairo_status_t result = cairo_status(cr);
    if (result == CAIRO_STATUS_SUCCESS) result = cairo_surface_write_to_png(surface, path);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    char *message = result == CAIRO_STATUS_SUCCESS ? g_strdup_printf("Screenshot saved: %s", path) :
        g_strdup_printf("Screenshot failed: %s (%s)", cairo_status_to_string(result), path);
    status(a, message);
    g_free(message); g_free(path); g_free(name); g_free(timestamp); g_date_time_unref(now);
}

static void show_config(GtkWidget *widget, gpointer data);
static void show_mode(GtkWidget *widget, gpointer data);
static void show_renderer(GtkWidget *widget, gpointer data);
static void show_about(GtkWidget *widget, gpointer data);
static void show_tci(GtkWidget *widget, gpointer data);

static void usb_source_refresh(App *a) {
    const char *section=sources[a->source];
    double center=g_key_file_get_double(a->config,section,"frequency",NULL)
        +g_key_file_get_double(a->config,section,"lo_offset",NULL);
    double rate=g_key_file_get_double(a->config,section,"sample_rate",NULL);
    usb_receiver_source(a->usb_receiver,center,rate,a->running && !a->stopping);
}
static void monitor_refresh(App *a) {
    a->iq_view.usb_enabled=usb_receiver_running(a->usb_receiver);
    a->iq_view.monitor_mode=usb_receiver_mode(a->usb_receiver);
    a->iq_view.usb_frequency=usb_receiver_frequency(a->usb_receiver);
    gtk_widget_set_sensitive(a->usb_port,!a->iq_view.usb_enabled);
    gtk_widget_queue_draw(a->iq_view.area);
}
static void usb_toggled(GtkToggleButton *button, gpointer data) {
    App *a=data;
    gboolean active=gtk_toggle_button_get_active(button);
    MonitorMode mode=GTK_WIDGET(button)==a->fm_data_toggle ? MONITOR_FM_DATA :
        GTK_WIDGET(button)==a->fm_toggle ? MONITOR_FM : MONITOR_USB;
    MonitorMode previous=usb_receiver_mode(a->usb_receiver);
    if (active) {
        usb_source_refresh(a);
        GError *error=NULL;
        gboolean ok=usb_receiver_set_mode(a->usb_receiver,mode);
        if (!ok) g_set_error_literal(&error,G_FILE_ERROR,G_FILE_ERROR_INVAL,
            "The selected channel does not fit in the IQ spectrum; adjust monitor frequency or input sample rate.");
        if (ok) {
            char *host=setting(a,"usb","tci_bind"), *device=setting(a,"usb","audio_device");
            unsigned port=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->usb_port));
            ok=usb_receiver_start(a->usb_receiver,host,port,device,&error);
            g_free(host); g_free(device);
        }
        if (!ok) {
            usb_receiver_set_mode(a->usb_receiver,previous);
            g_signal_handlers_block_by_func(button,usb_toggled,a);
            gtk_toggle_button_set_active(button,FALSE);
            g_signal_handlers_unblock_by_func(button,usb_toggled,a);
            gtk_label_set_text(GTK_LABEL(a->usb_status),error->message);
            status(a,error->message); g_clear_error(&error); monitor_refresh(a); return;
        }
        /* Switch DSP in the existing receiver. Keep the speaker, TCI clients
         * and DVB processes alive while changing modulation. */
        GtkWidget *buttons[]={a->usb_toggle,a->fm_toggle,a->fm_data_toggle};
        for (unsigned i=0;i<G_N_ELEMENTS(buttons);++i) {
            GtkWidget *other=buttons[i];
            if (other==GTK_WIDGET(button)) continue;
            g_signal_handlers_block_by_func(other,usb_toggled,a);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(other),FALSE);
            g_signal_handlers_unblock_by_func(other,usb_toggled,a);
        }
    } else {
        usb_receiver_stop(a->usb_receiver);
        gtk_label_set_text(GTK_LABEL(a->usb_status),"USB / FM / FM-DATA off");
    }
    monitor_refresh(a);
    gtk_widget_set_tooltip_text(a->usb_toggle,"USB (U) · 3 kHz · Page Up / Page Down: -100 / +100 Hz");
    gtk_widget_set_tooltip_text(a->fm_toggle,"FM (F) · 12.5 kHz · Page Up / Page Down: -2.5 / +2.5 kHz");
    gtk_widget_set_tooltip_text(a->fm_data_toggle,"FM-DATA (D) · 20 kHz · flat audio · Page Up / Page Down: -2.5 / +2.5 kHz");
}
static void usb_port_changed(GtkSpinButton *spin, gpointer data) {
    App *a=data;
    g_key_file_set_integer(a->config,"usb","tci_port",gtk_spin_button_get_value_as_int(spin));
}
static void usb_tune(App *a, double frequency) {
    if (!a->usb_receiver) return;
    usb_source_refresh(a);
    if (!usb_receiver_tune(a->usb_receiver,frequency)) {
        status(a,"Monitor tuning limit reached within the current IQ spectrum"); return;
    }
    a->iq_view.usb_frequency=frequency;
    gtk_widget_queue_draw(a->iq_view.area);
}
static void usb_tune_step(App *a, int direction) {
    usb_source_refresh(a);
    double step=usb_receiver_mode(a->usb_receiver)==MONITOR_USB ? 100 : 2500;
    usb_tune(a,usb_receiver_frequency(a->usb_receiver)+direction*step);
}

static gboolean usb_spectrum_clicked(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    App *a=data;
    if (!usb_receiver_running(a->usb_receiver) || event->type!=GDK_BUTTON_PRESS || event->button!=1 ||
        (event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK))) return FALSE;
    /* Use the same plot bounds and displayed frequency scale as iq_view_draw.
     * Axes, waterfall and overlaid controls do not select a USB frequency. */
    double left=65, right=gtk_widget_get_allocated_width(widget)-40;
    double height=gtk_widget_get_allocated_height(widget), top=height/2+25, bottom=height-30;
    double rate=a->iq_view.rate;
    if (!isfinite(event->x) || !isfinite(event->y) || !isfinite(rate) || rate<=0 ||
        right<=left || bottom<=top || event->x<left || event->x>right || event->y<top || event->y>bottom)
        return FALSE;
    double frequency=a->iq_view.center-rate/2+(event->x-left)/(right-left)*rate;
    usb_tune(a,round(frequency));
    return TRUE;
}

static gboolean tuning_key(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    App *a = data; (void)widget;
    if (event->state & (GDK_CONTROL_MASK | GDK_SUPER_MASK)) return FALSE;
    if (event->state & GDK_MOD1_MASK) {
        switch (gdk_keyval_to_lower(event->keyval)) {
        case GDK_KEY_i: show_config(NULL,a); return TRUE;
        case GDK_KEY_m: show_mode(NULL,a); return TRUE;
        case GDK_KEY_r: show_renderer(NULL,a); return TRUE;
        case GDK_KEY_t: show_tci(NULL,a); return TRUE;
        case GDK_KEY_a: show_about(NULL,a); return TRUE;
        default: return FALSE;
        }
    }
    switch (gdk_keyval_to_lower(event->keyval)) {
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up:
        usb_tune_step(a,-1); return TRUE;
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down:
        usb_tune_step(a,1); return TRUE;
    case GDK_KEY_u:
        if (a->usb_toggle) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->usb_toggle),
            !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->usb_toggle)));
        return TRUE;
    case GDK_KEY_f:
        if (a->fm_toggle) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->fm_toggle),
            !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->fm_toggle)));
        return TRUE;
    case GDK_KEY_d:
        if (a->fm_data_toggle) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->fm_data_toggle),
            !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->fm_data_toggle)));
        return TRUE;
    case GDK_KEY_m:
        video_view_set_muted(&a->video_view, !a->video_view.muted);
        usb_receiver_set_muted(a->usb_receiver,a->video_view.muted);
        return TRUE;
    case GDK_KEY_r:
        if (a->running && !a->stopping && a->scatter && a->scatter_backend) {
            scatter_backend_reset(a->scatter_backend);
            scatter_view_reset(&a->scatter_view);
            status(a, "TSscatterFix reset; waiting for new TS data.");
        }
        return TRUE;
    case GDK_KEY_p: save_screenshot(a); return TRUE;
    default: break;
    }
    if (event->keyval == GDK_KEY_w || event->keyval == GDK_KEY_W) { save_settings(a); return TRUE; }
    if (event->keyval == GDK_KEY_bracketleft) { filter_step(a, -1); return TRUE; }
    if (event->keyval == GDK_KEY_bracketright) { filter_step(a, 1); return TRUE; }
    /* Up/down remain dedicated SDR gain controls. */
    if (event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up) {
        if (hardware_source(a)) gain_step(a,1);
        return hardware_source(a);
    }
    if (event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down) {
        if (hardware_source(a)) gain_step(a,-1);
        return hardware_source(a);
    }
    if (!hardware_source(a)) return FALSE;
    /* Handle before GTK propagates to focused entries: plain arrows tune
     * throughout this window; modified arrows retain normal editing behavior. */
    if (event->keyval == GDK_KEY_Left || event->keyval == GDK_KEY_KP_Left) { tune_step(a, -1); return TRUE; }
    if (event->keyval == GDK_KEY_Right || event->keyval == GDK_KEY_KP_Right) { tune_step(a, 1); return TRUE; }
    return FALSE;
}

static void source_changed(GtkWidget *widget, gpointer data) {
    App *a = data;
    if (a->running) return;
    a->source = gtk_combo_box_get_active(GTK_COMBO_BOX(a->source_box));
    const char *section = sources[a->source];
    if (widget == a->source_box) {
        g_signal_handlers_block_by_func(a->format_box, source_changed, a);
        char *fmt = setting(a, section, "format");
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->format_box), !strcmp(fmt, "u8")); g_free(fmt);
        g_signal_handlers_unblock_by_func(a->format_box, source_changed, a);
        g_signal_handlers_block_by_func(a->rate_box, source_changed, a);
        g_signal_handlers_block_by_func(a->frequency_box, source_changed, a);
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(a->frequency_box),
            rx_frequency_min(section)/1000.0, rx_frequency_max(section)/1000.0);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(a->frequency_box), 3);
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(a->rate_box), a->source == SOURCE_AIRSPYR2 ? 2500000 : a->source == SOURCE_AIRSPYHF ? 1000 : rx_rsp(sources[a->source]) ? 62500 : a->source == SOURCE_LIME ? LIME_RX_MIN_RATE : a->source == SOURCE_FCDPP ? FUNCUBE_RATE : a->source == SOURCE_RTLSDR ? 225001 : a->source == SOURCE_AFEDRI ? 48000 : a->source == 3 ? 8138 : hardware_source(a) ? 2000000 : 1,
                                  a->source == SOURCE_AIRSPYR2 ? 10000000 : a->source == SOURCE_AIRSPYHF ? 768000 : rx_rsp(sources[a->source]) ? 10000000 : a->source == SOURCE_LIME ? LIME_RX_MAX_RATE : a->source == SOURCE_FCDPP ? FUNCUBE_RATE : a->source == SOURCE_RTLSDR ? 3200000 : a->source == SOURCE_AFEDRI ? 2400000 : a->source == 3 ? 196078 : a->source == 1 ? 20000000 : a->source == 2 ? 61440000 : 100000000);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->rate_box),
            integer_setting(a, section, "sample_rate", 1, 100000000));
        if (a->source == 3 || a->source == SOURCE_RTLSDR || a->source == SOURCE_FCDPP || a->source == SOURCE_AIRSPYR2) {
            g_signal_handlers_block_by_func(a->sdriq_rate_box, source_changed, a);
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(a->sdriq_rate_box));
            const int *rates = a->source == SOURCE_AIRSPYR2 ? airspyr2_rates : a->source == SOURCE_FCDPP ? funcube_rates : a->source == SOURCE_RTLSDR ? rtlsdr_rates : sdriq_rates;
            size_t count = a->source == SOURCE_AIRSPYR2 ? G_N_ELEMENTS(airspyr2_rates) : a->source == SOURCE_FCDPP ? G_N_ELEMENTS(funcube_rates) : a->source == SOURCE_RTLSDR ? G_N_ELEMENTS(rtlsdr_rates) : G_N_ELEMENTS(sdriq_rates);
            int configured = g_key_file_get_integer(a->config, section, "sample_rate", NULL);
            gboolean found = FALSE;
            for (size_t i = 0; i < count; ++i) {
                char *id = g_strdup_printf("%d", rates[i]);
                gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(a->sdriq_rate_box), id, id);
                g_free(id); found |= rates[i] == configured;
            }
            if (!found) {
                char *id = g_strdup_printf("%d", configured);
                gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(a->sdriq_rate_box), id, id); g_free(id);
            }
            char *rate_id = setting(a, section, "sample_rate");
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(a->sdriq_rate_box), rate_id); g_free(rate_id);
            g_signal_handlers_unblock_by_func(a->sdriq_rate_box, source_changed, a);
        }
        if (hardware_source(a)) {
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->frequency_box), g_key_file_get_double(a->config, section, "frequency", NULL) / 1000.0);
        }
        g_signal_handlers_unblock_by_func(a->rate_box, source_changed, a);
        g_signal_handlers_unblock_by_func(a->frequency_box, source_changed, a);
    } else {
        g_key_file_set_string(a->config, section, "format", gtk_combo_box_get_active(GTK_COMBO_BOX(a->format_box)) ? "u8" : "s16");
        g_key_file_set_integer(a->config, section, "sample_rate", (a->source == 3 || a->source == SOURCE_RTLSDR || a->source == SOURCE_FCDPP || a->source == SOURCE_AIRSPYR2) ?
            atoi(gtk_combo_box_get_active_id(GTK_COMBO_BOX(a->sdriq_rate_box))) :
            gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->rate_box)));
        if (hardware_source(a)) {
            g_key_file_set_double(a->config, section, "frequency", gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->frequency_box)) * 1000.0);
        }
    }
    gtk_widget_set_visible(a->rate_box, a->source != 3 && a->source != SOURCE_RTLSDR && a->source != SOURCE_FCDPP && a->source != SOURCE_AIRSPYR2);
    gtk_widget_set_visible(a->sdriq_rate_box, a->source == 3 || a->source == SOURCE_RTLSDR || a->source == SOURCE_FCDPP || a->source == SOURCE_AIRSPYR2);
    gtk_widget_set_sensitive(a->frequency_box, hardware_source(a));
    gtk_widget_set_sensitive(a->format_box, a->source!=SOURCE_FILE);
    if (a->hardware_stack) gtk_stack_set_visible_child_name(GTK_STACK(a->hardware_stack), section);
    if (a->file_path) file_controls_refresh(a);
    tuning_refresh(a);
    filter_refresh(a);
    refresh_preview(a);
    power_refresh(a);
}

static void selection_changed(GtkWidget *widget, gpointer data) {
    App *a = data; (void)widget;
    if (a->running) return;
    char *sr = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(a->sr_box));
    if (!sr) return;
    a->sr = atoi(sr); g_free(sr);
    const char *fec_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(a->fec_box));
    if (!fec_id) return;
    a->fec = atoi(fec_id);
    a->gi = gtk_combo_box_get_active(GTK_COMBO_BOX(a->gi_box));
    a->scatter = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->scatter_box));
    a->renderer = gtk_combo_box_get_active(GTK_COMBO_BOX(a->renderer_box));
    refresh_preview(a);
}

static void scatter_changed(GtkWidget *widget, gpointer data) {
    App *a=data;
    if (a->stopping) {
        g_signal_handlers_block_by_func(widget, scatter_changed, a);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget),a->scatter);
        g_signal_handlers_unblock_by_func(widget, scatter_changed, a);
        return;
    }
    a->scatter = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    scatter_backend_set_enabled(a->scatter_backend, a->scatter);
    scatter_view_reset(&a->scatter_view);
    scatter_refresh(a);
    /* Do not rebuild stages while their PIDs/process groups are live. */
    char *text = describe(a);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->preview)), text, -1);
    g_free(text);
}

static void populate_rates(App *a) {
    if (a->sr_label) gtk_label_set_text(GTK_LABEL(a->sr_label), a->standard == 2 ? "BW (Hz)" : "Symbol rate (sym/s)");
    const int *rates = a->standard == 2 ? terrestrial_rates : sat_rates;
    int count = a->standard == 2 ? (int)G_N_ELEMENTS(terrestrial_rates) : (int)G_N_ELEMENTS(sat_rates), active = -1;
    g_signal_handlers_block_by_func(a->sr_box, selection_changed, a);
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(a->sr_box));
    for (int i = 0; i < count; ++i) {
        char *label = g_strdup_printf("%d", rates[i]);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->sr_box), label); g_free(label);
        if (rates[i] == a->sr) active = i;
    }
    if (active < 0 && a->source==SOURCE_FILE && a->sr>0) {
        char *label=g_strdup_printf("%d",a->sr);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(a->sr_box),label); g_free(label);
        active=count;
    }
    if (active < 0) active = a->standard == 2 ? 0 : 2;
    if (active<count) a->sr = rates[active];
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->sr_box), active);
    gtk_widget_set_sensitive(a->gi_box, a->standard == 2);
    g_signal_handlers_unblock_by_func(a->sr_box, selection_changed, a);
}

/* Keep an incompatible saved selection visible so the user can correct it;
 * prevent choosing physically impossible rates from the popup. Start and CLI
 * validation use the same bandwidth calculation, including zero offset. */
static void rate_cell_data(GtkCellLayout *layout, GtkCellRenderer *cell,
        GtkTreeModel *model, GtkTreeIter *iter, gpointer data) {
    (void)layout; App *a=data;
    char *label=NULL;
    gtk_tree_model_get(model,iter,0,&label,-1);
    double width=label ? g_ascii_strtod(label,NULL)*(a->standard == 2 ? 1 : 1.35) : NAN;
    gboolean valid=!hardware_source(a) || rx_bandwidth_fits(sources[a->source],
        g_key_file_get_double(a->config,sources[a->source],"sample_rate",NULL),receiver_offset(a),width);
    g_object_set(cell,"sensitive",valid,NULL);
    g_free(label);
}

static void populate_fecs(App *a) {
    g_signal_handlers_block_by_func(a->fec_box, selection_changed, a);
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(a->fec_box));
    if (!valid_fec(a, a->fec)) a->fec = a->standard == 1 && a->constellation ? 1 : 0;
    for (int i = 0; i < (int)G_N_ELEMENTS(fecs); ++i) {
        if (!valid_fec(a, i)) continue;
        char *id = g_strdup_printf("%d", i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(a->fec_box), id, fecs[i]);
        if (i == a->fec) gtk_combo_box_set_active_id(GTK_COMBO_BOX(a->fec_box), id);
        g_free(id);
    }
    gtk_widget_set_sensitive(a->constellation_box, a->standard == 1);
    if (a->ldpc_mode_box) {
        gboolean soft=gtk_combo_box_get_active(GTK_COMBO_BOX(a->ldpc_mode_box))==0;
        gtk_widget_set_sensitive(a->ldpc_mode_box,a->standard==1);
        gtk_widget_set_sensitive(a->ldpc_workers_box,a->standard==1 && soft);
        gtk_widget_set_sensitive(a->ldpc_latency_box,a->standard==1 && soft);
        gtk_widget_set_sensitive(a->s2_acquisition_box,a->standard==1);
    }
    g_signal_handlers_unblock_by_func(a->fec_box, selection_changed, a);
}

static void constellation_changed(GtkWidget *widget, gpointer data) {
    App *a = data;
    if (a->running) return;
    a->constellation = gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
    populate_fecs(a); refresh_preview(a);
}

static void standard_changed(GtkWidget *widget, gpointer data) {
    App *a = data; a->standard = gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
    populate_rates(a); populate_fecs(a); refresh_preview(a);
}
static void power_clicked(GtkWidget *w, gpointer data) {
    (void)w; App *a = data;
    if (a->stopping) return;
    if (a->running) stop_pipeline(a); else start_pipeline(a);
    power_refresh(a);
}
static gboolean window_close(GtkWidget *w, GdkEvent *event, gpointer data) {
    (void)w; (void)event; App *a = data; a->closing = TRUE;
    if (a->running) stop_pipeline(a); else gtk_main_quit();
    return TRUE;
}
static gboolean exit_signal(gpointer data) {
    App *a = data; a->closing = TRUE;
    if (a->running) stop_pipeline(a);
    else if (a->headless) g_main_loop_quit(a->loop);
    else gtk_main_quit();
    return G_SOURCE_CONTINUE;
}

static void select_program(guint16 service, gpointer data) {
    App *a = data;
    if (!a->running || a->stopping || a->renderer != RENDER_EMBEDDED || !a->scatter_backend) return;
    TsInfoSnapshot snapshot = {0};
    ts_info_backend_snapshot(a->scatter_backend->information, &snapshot);
    gboolean found = FALSE;
    for (unsigned i = 0; i < snapshot.count; ++i) if (snapshot.services[i].id == service) found = TRUE;
    ts_info_snapshot_clear(&snapshot);
    if (!found) return;
    GError *error = NULL;
    if (!video_view_select_program(&a->video_view, service, &error)) {
        status(a, error->message); g_clear_error(&error);
        stop_pipeline(a);
        return;
    }
    ts_info_backend_select(a->scatter_backend->information, service);
    a->information.next_tick = 0;
    ts_info_view_tick(&a->information, a->scatter_backend->information);
}

static gboolean decoder_tick(gpointer data) {
    App *a=data;
    ts_info_view_tick(&a->information, a->running && !a->stopping && a->scatter_backend ?
        a->scatter_backend->information : NULL);
    if (!a->stopping) scatter_view_tick(&a->scatter_view, a->scatter_backend);
    if (a->running && !a->stopping && (video_view_failed(&a->video_view) ||
        (a->scatter_backend && g_atomic_int_get(&a->scatter_backend->relay_failed)))) {
        a->exit_code = 1; status(a, "Video / UDP output failed; check the VLC plugins and received stream.");
        stop_pipeline(a);
    }
    {
        a->decoder_view.selected_service[0]=a->decoder_view.selected_provider[0]=0;
        a->decoder_view.selected_video_codec[0]=a->decoder_view.selected_audio_codec[0]=0;
        a->decoder_view.service_valid=FALSE;
        if (a->running && !a->stopping && a->scatter_backend) {
            TsInfoSnapshot snapshot={0};
            ts_info_backend_snapshot(a->scatter_backend->information,&snapshot);
            gboolean can_select = a->renderer == RENDER_EMBEDDED && a->video_view.active;
            if (can_select && !a->scatter && snapshot.count) {
                gboolean found = FALSE;
                for (unsigned i = 0; i < snapshot.count; ++i)
                    if (snapshot.services[i].id == a->video_view.program) found = TRUE;
                if (!found) {
                    select_program(snapshot.services[0].id, a);
                    ts_info_snapshot_clear(&snapshot);
                    ts_info_backend_snapshot(a->scatter_backend ? a->scatter_backend->information : NULL, &snapshot);
                }
            }
            program_view_update(&a->programs, &snapshot, a->video_view.program, can_select && !a->stopping);
            g_strlcpy(a->decoder_view.selected_video_codec,snapshot.video_codec,sizeof(a->decoder_view.selected_video_codec));
            g_strlcpy(a->decoder_view.selected_audio_codec,snapshot.audio_codec,sizeof(a->decoder_view.selected_audio_codec));
            for (unsigned i=0;i<snapshot.count;++i) if (snapshot.services[i].id==snapshot.selected) {
                g_strlcpy(a->decoder_view.selected_service,snapshot.services[i].name,sizeof(a->decoder_view.selected_service));
                g_strlcpy(a->decoder_view.selected_provider,snapshot.services[i].provider,sizeof(a->decoder_view.selected_provider));
                a->decoder_view.service_valid=snapshot.pmt_seen;
            }
            ts_info_snapshot_clear(&snapshot);
        }
    }
    decoder_view_tick(&a->decoder_view, a->running && !a->stopping, standards[a->standard]);
    return G_SOURCE_CONTINUE;
}

static gboolean monitor_tick(gpointer data) {
    App *a = data;
    const char *section = sources[a->source];
    a->iq_view.active = a->running && !a->stopping;
    a->iq_view.bandwidth = g_key_file_get_double(a->config, section, "filter_bandwidth", NULL);
    a->iq_view.enabled = g_key_file_get_boolean(a->config, section, "filter_enabled", NULL);
    a->iq_view.receiver_offset = receiver_offset(a);
    double center = g_key_file_get_double(a->config, section, "frequency", NULL)
        + g_key_file_get_double(a->config, section, "lo_offset", NULL);
    double rate = g_key_file_get_double(a->config, section, "sample_rate", NULL);
    if (center != a->iq_view.center || rate != a->iq_view.rate) {
        a->iq_view.rows = 0; a->iq_view.last_frame = 0;
    }
    a->iq_view.center = center; a->iq_view.rate = rate;
    usb_source_refresh(a);
    a->iq_view.usb_enabled=usb_receiver_running(a->usb_receiver);
    a->iq_view.monitor_mode=usb_receiver_mode(a->usb_receiver);
    a->iq_view.usb_frequency=usb_receiver_frequency(a->usb_receiver);
    if (a->usb_status && usb_receiver_running(a->usb_receiver)) {
        char message[512]; usb_receiver_status(a->usb_receiver,message,sizeof(message));
        gtk_label_set_text(GTK_LABEL(a->usb_status),message);
        MonitorMode mode=a->iq_view.monitor_mode;
        char *tip=g_strdup_printf("%s · Page Up / Page Down: %s\n%s",monitor_name(mode),
            mode==MONITOR_USB ? "-100 / +100 Hz" : "-2.5 / +2.5 kHz",message);
        GtkWidget *button=mode==MONITOR_FM_DATA ? a->fm_data_toggle : mode==MONITOR_FM ? a->fm_toggle : a->usb_toggle;
        gtk_widget_set_tooltip_text(button,tip); g_free(tip);
    }
    iq_view_receive(&a->iq_view);
    return G_SOURCE_CONTINUE;
}

static GtkWidget *combo(GtkWidget *grid, const char *label, int row, const char **values, int n, int active) {
    GtkWidget *l = gtk_label_new(label), *box = gtk_combo_box_text_new();
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1); gtk_grid_attach(GTK_GRID(grid), box, 1, row, 1, 1);
    for (int i = 0; i < n; ++i) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(box), values[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(box), active); return box;
}

static GtkWidget *text_panel(GtkWidget *parent, const char *title, int height) {
    GtkWidget *frame = gtk_frame_new(title), *scroll = gtk_scrolled_window_new(NULL, NULL), *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE); gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE); gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scroll), view); gtk_container_add(GTK_CONTAINER(frame), scroll);
    gtk_widget_set_size_request(frame, -1, height); gtk_box_pack_start(GTK_BOX(parent), frame, TRUE, TRUE, 0);
    return view;
}

static GtkWidget *spin(GtkWidget *grid, const char *label, int row, double low, double high, double step) {
    GtkWidget *l = gtk_label_new(label), *w = gtk_spin_button_new_with_range(low, high, step);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
    return w;
}

/* Keep older Linrad-only configuration files usable. */
static void source_defaults(App *a) {
    const char *lean_keys[]={"backend","ldpc_mode","helpers","latency","acquisition"};
    const char *lean_values[]={"embedded","soft","1","auto","auto"};
    for (unsigned i=0;i<G_N_ELEMENTS(lean_keys);++i)
        if (!g_key_file_has_key(a->config,"leandvb",lean_keys[i],NULL))
            g_key_file_set_string(a->config,"leandvb",lean_keys[i],lean_values[i]);
    const char *usb_keys[]={"tci_bind","tci_port","audio_device","pcm_udp_enabled","pcm_udp_host","pcm_udp_port"};
    const char *usb_values[]={"127.0.0.1","40031","default","false","127.0.0.1","7355"};
    for (unsigned i=0;i<G_N_ELEMENTS(usb_keys);++i)
        if (!g_key_file_has_key(a->config,"usb",usb_keys[i],NULL))
            g_key_file_set_string(a->config,"usb",usb_keys[i],usb_values[i]);
    const char *udp_keys[] = {"udp_enabled", "udp_host", "udp_port"};
    const char *udp_values[] = {"false", "127.0.0.1", "1234"};
    for (unsigned i = 0; i < G_N_ELEMENTS(udp_keys); ++i)
        if (!g_key_file_has_key(a->config, "renderer", udp_keys[i], NULL))
            g_key_file_set_string(a->config, "renderer", udp_keys[i], udp_values[i]);
    const char *file_keys[]={"path","input_format","format","sample_rate","header_bytes","frequency","metadata"};
    const char *file_values[]={"","s16","s16","250000","0","0","No metadata loaded; check the settings."};
    for (unsigned i=0; i<G_N_ELEMENTS(file_keys); ++i)
        if (!g_key_file_has_key(a->config,"file",file_keys[i],NULL))
            g_key_file_set_string(a->config,"file",file_keys[i],file_values[i]);
    if (!g_key_file_has_key(a->config,"paths","iq_file",NULL)) {
        char *exe=g_file_read_link("/proc/self/exe",NULL);
        char *dir=exe ? g_path_get_dirname(exe) : g_build_filename(a->directory,"build",NULL);
        char *player=g_build_filename(dir,"iq_file_to_stdout",NULL);
        g_key_file_set_string(a->config,"paths","iq_file",player);
        g_free(exe); g_free(dir); g_free(player);
    }
    const char *keys[] = {"sample_rate", "frequency", "gain", "args", "images", "antenna", "format"};
    const char *values[] = {"10000000", "437000000", "30", "", "", "TX/RX", "s16"};
    const char *sdriq_values[] = {"111111", "28000000", "10", "driver=rfspace,sdr-iq=/dev/ttyUSB0", "", "", "s16"};
    const char *rtlsdr_values[] = {"2400000", "437000000", "30", "driver=rtlsdr", "", "RX", "u8"};
    const char *lime_values[] = {"10000000", "437000000", "30", "", "", "AUTO", "s16"};
    const char *funcube_values[] = {"192000", "437000000", "20", "lna=1,mixer=1", "", "", "s16"};
    const char *rsp_values[] = {"2000000", "144600000", "40", "driver=sdrplay", "", "", "s16"};
    const char *airspyr2_values[] = {"10000000", "437000000", "30", "driver=airspy", "", "RX", "s16"};
    const char *airspyhf_values[] = {"768000", "144600000", "0", "driver=airspyhf", "", "", "s16"};
    const char *afedri_values[] = {"1010526", "28000000", "14", "", "", "", "s16"};
    const char *net_keys[] = {"ip", "port", "bind"};
    const char *net_values[] = {"192.168.0.118", "50005", "0.0.0.0"};
    for (int i = 0; i < 3; ++i)
        if (!g_key_file_has_key(a->config, "afedri", net_keys[i], NULL))
            g_key_file_set_string(a->config, "afedri", net_keys[i], net_values[i]);
    for (int j = 1; j < SOURCE_FILE; ++j)
        for (int i = 0; i < 7; ++i)
            if (!g_key_file_has_key(a->config, sources[j], keys[i], NULL))
                g_key_file_set_string(a->config, sources[j], keys[i], j == SOURCE_AIRSPYR2 ? airspyr2_values[i] : j == SOURCE_AIRSPYHF ? airspyhf_values[i] : rx_rsp(sources[j]) ? rsp_values[i] : j == SOURCE_LIME ? lime_values[i] : j == SOURCE_FCDPP ? funcube_values[i] : j == SOURCE_RTLSDR ? rtlsdr_values[i] : j == SOURCE_AFEDRI ? afedri_values[i] : j == 3 ? sdriq_values[i] : i == 6 && j == 1 ? "u8" : values[i]);
    for (int j = 1; j < SOURCE_FILE; ++j) {
        if (!g_key_file_has_key(a->config,sources[j],"receiver_offset",NULL))
            g_key_file_set_double(a->config,sources[j],"receiver_offset",0);
        if (!g_key_file_has_key(a->config, sources[j], "lo_offset", NULL))
            g_key_file_set_double(a->config, sources[j], "lo_offset", 0);
        if (!g_key_file_has_key(a->config, sources[j], "tune_step", NULL))
            g_key_file_set_double(a->config, sources[j], "tune_step", 1000);
    }
    for (int j = 0; j < (int)G_N_ELEMENTS(sources); ++j) {
        if (!g_key_file_has_key(a->config, sources[j], "filter_bandwidth", NULL))
            g_key_file_set_integer(a->config, sources[j], "filter_bandwidth", 50000);
        if (!g_key_file_has_key(a->config, sources[j], "filter_enabled", NULL))
            g_key_file_set_boolean(a->config, sources[j], "filter_enabled", FALSE);
    }
    if (!g_key_file_has_key(a->config, "paths", "sdr", NULL))
        g_key_file_set_string(a->config, "paths", "sdr", "build/sdr_to_stdout");
    if (!g_key_file_has_key(a->config, "paths", "vlc", NULL))
        g_key_file_set_string(a->config, "paths", "vlc", "vlc");
    if (!g_key_file_has_key(a->config, "paths", "ffplay", NULL))
        g_key_file_set_string(a->config, "paths", "ffplay", "ffplay");
    if (!g_key_file_has_key(a->config, "ffplay", "args", NULL))
        g_key_file_set_string(a->config, "ffplay", "args", ffplay_args);
    if (!g_key_file_has_key(a->config, "ffplay", "gpu_program", NULL))
        g_key_file_set_string(a->config, "ffplay", "gpu_program", "/usr/bin/ffplay");
    if (!g_key_file_has_key(a->config, "ffplay", "gpu_device", NULL))
        g_key_file_set_string(a->config, "ffplay", "gpu_device", "NVIDIA");
    if (!g_key_file_has_key(a->config, "paths", "custom_renderer", NULL))
        g_key_file_set_string(a->config, "paths", "custom_renderer", "mpv");
    if (!g_key_file_has_key(a->config, "renderer", "args", NULL))
        g_key_file_set_string(a->config, "renderer", "args", "{input}");
    if (!g_key_file_has_key(a->config, "renderer", "output_file", NULL))
        g_key_file_set_string(a->config, "renderer", "output_file", "capture.ts");
}

static int find_choice(const char *value, const char **choices, unsigned count) {
    for (unsigned i=0; i<count; ++i) if (!strcmp(value,choices[i])) return (int)i;
    return -1;
}

/* Validate everything first; selecting a bad file leaves the current receiver intact. */
static gboolean select_iq_file(App *a, const char *path, GError **error) {
    IQFileInfo info;
    if (!iq_file_read_info(path,&info,error)) return FALSE;
    int standard=*info.standard ? find_choice(info.standard,standards,G_N_ELEMENTS(standards)) : a->standard;
    int constellation=*info.constellation ? find_choice(info.constellation,constellations,G_N_ELEMENTS(constellations)) : 0;
    int fec=*info.fec ? find_choice(info.fec,fecs,G_N_ELEMENTS(fecs)) : 0;
    int gi=*info.gi ? find_choice(info.gi,guards,G_N_ELEMENTS(guards)) : 0;
    int sr=info.symbol_rate ? info.symbol_rate : standard!=a->standard ? (standard==2 ? 150000 : 125000) : a->sr;
    App candidate={.standard=standard,.constellation=constellation,.fec=fec};
    if (standard<0 || constellation<0 || fec<0 || gi<0 ||
        (standard!=1 && constellation!=0) || !valid_fec(&candidate,fec)) {
        g_set_error_literal(error,G_OPTION_ERROR,G_OPTION_ERROR_BAD_VALUE,"Unsupported DVB mode, modulation, FEC or guard interval in metadata");
        iq_file_info_clear(&info); return FALSE;
    }
    char *fallback=setting(a,"file","input_format");
    const char *format=*info.format ? info.format : fallback;
    int rate=info.sample_rate ? info.sample_rate : integer_setting(a,"file","sample_rate",1,100000000);
    guint64 samples;
    if (rate<1 || rate>100000000 || !iq_file_validate(info.path,format,info.header_bytes,&samples,error)) {
        if (error && !*error) g_set_error_literal(error,G_OPTION_ERROR,G_OPTION_ERROR_BAD_VALUE,"Invalid sample rate");
        g_free(fallback); iq_file_info_clear(&info); return FALSE;
    }
    g_key_file_set_string(a->config,"file","path",info.path);
    g_key_file_set_string(a->config,"file","input_format",format);
    g_key_file_set_string(a->config,"file","format",!strcmp(format,"u8") ? "u8" : "s16");
    g_key_file_set_integer(a->config,"file","sample_rate",rate);
    g_key_file_set_integer(a->config,"file","header_bytes",info.header_bytes);
    g_key_file_set_double(a->config,"file","frequency",info.has_frequency ? info.frequency : 0);
    g_key_file_set_boolean(a->config,"file","filter_enabled",FALSE);
    g_key_file_set_integer(a->config,"file","filter_bandwidth",CLAMP((int)MIN(sr*1.35,100000000),MAX(10,(rate+255)/256),MAX(10,(int)(rate*.95))));
    char *message=info.metadata_path ? g_strdup_printf("Metadata: %s%s",info.metadata_path,
        !info.sample_rate || !*info.format ? " · check sample rate and format" : "") :
        g_strdup("No JSON metadata. Check the sample rate, format and DVB settings before playback.");
    g_key_file_set_string(a->config,"file","metadata",message); g_free(message);
    a->source=SOURCE_FILE; a->standard=standard; a->constellation=constellation; a->fec=fec; a->gi=gi; a->sr=sr;
    g_free(fallback); iq_file_info_clear(&info); return TRUE;
}

static void file_options_changed(GtkWidget *widget, gpointer data);
static void file_controls_refresh(App *a) {
    GtkWidget *widgets[]={a->file_format,a->file_header,a->file_center};
    for (unsigned i=0; i<G_N_ELEMENTS(widgets); ++i) g_signal_handlers_block_by_func(widgets[i],file_options_changed,a);
    char *path=setting(a,"file","path"), *format=setting(a,"file","input_format"), *metadata=setting(a,"file","metadata");
    gtk_entry_set_text(GTK_ENTRY(a->file_path),path);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->file_format),find_choice(format,file_formats,G_N_ELEMENTS(file_formats)));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->file_header),integer_setting(a,"file","header_bytes",0,100000000));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->file_center),g_key_file_get_double(a->config,"file","frequency",NULL)/1e6);
    GError *error=NULL; guint64 samples;
    char *resolved=g_canonicalize_filename(path,a->directory), *text;
    int rate=integer_setting(a,"file","sample_rate",1,100000000);
    if (*path && iq_file_validate(resolved,format,gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->file_header)),&samples,&error))
        text=g_strdup_printf("%.3f s · %d samples/s · %s → %s\n%s",(double)samples/rate,rate,format,!strcmp(format,"u8") ? "u8" : "s16",metadata);
    else text=g_strdup(error ? error->message : "Select an IQ file. Click Play file at the top right to start playback.");
    gtk_label_set_text(GTK_LABEL(a->file_details),text);
    for (unsigned i=0; i<G_N_ELEMENTS(widgets); ++i) g_signal_handlers_unblock_by_func(widgets[i],file_options_changed,a);
    g_clear_error(&error); g_free(text); g_free(path); g_free(resolved); g_free(format); g_free(metadata);
}
static void file_options_changed(GtkWidget *widget, gpointer data) {
    (void)widget; App *a=data;
    if (a->running) return;
    int format=gtk_combo_box_get_active(GTK_COMBO_BOX(a->file_format));
    if (format<0) return;
    g_key_file_set_string(a->config,"file","input_format",file_formats[format]);
    g_key_file_set_string(a->config,"file","format",format==1 ? "u8" : "s16");
    g_key_file_set_integer(a->config,"file","header_bytes",gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->file_header)));
    g_key_file_set_double(a->config,"file","frequency",gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->file_center))*1e6);
    source_changed(a->source_box,a);
}
static void file_selection_sync(App *a) {
    g_signal_handlers_block_by_func(a->source_box,source_changed,a);
    g_signal_handlers_block_by_func(a->standard_box,standard_changed,a);
    g_signal_handlers_block_by_func(a->constellation_box,constellation_changed,a);
    g_signal_handlers_block_by_func(a->gi_box,selection_changed,a);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box),a->source);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->standard_box),a->standard);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->constellation_box),a->constellation);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->gi_box),a->gi);
    populate_rates(a); populate_fecs(a);
    g_signal_handlers_unblock_by_func(a->source_box,source_changed,a);
    g_signal_handlers_unblock_by_func(a->standard_box,standard_changed,a);
    g_signal_handlers_unblock_by_func(a->constellation_box,constellation_changed,a);
    g_signal_handlers_unblock_by_func(a->gi_box,selection_changed,a);
    source_changed(a->source_box,a);
}
static void choose_iq_file(GtkWidget *widget, gpointer data) {
    (void)widget; App *a=data;
    if (a->running) return;
    GtkWidget *chooser=gtk_file_chooser_dialog_new("Open IQ file",GTK_WINDOW(a->config_dialog),GTK_FILE_CHOOSER_ACTION_OPEN,
        "Cancel",GTK_RESPONSE_CANCEL,"Open",GTK_RESPONSE_ACCEPT,NULL);
    GtkFileFilter *filter=gtk_file_filter_new(); gtk_file_filter_set_name(filter,"IQ files");
    const char *patterns[]={"*.iq","*.raw","*.cf32","*.fc32","*.s16","*.u8"};
    for (unsigned i=0; i<G_N_ELEMENTS(patterns); ++i) gtk_file_filter_add_pattern(filter,patterns[i]);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser),filter);
    filter=gtk_file_filter_new(); gtk_file_filter_set_name(filter,"All files"); gtk_file_filter_add_pattern(filter,"*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser),filter);
    char *current=setting(a,"file","path");
    if (*current) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser),current);
    g_free(current);
    if (gtk_dialog_run(GTK_DIALOG(chooser))==GTK_RESPONSE_ACCEPT) {
        char *path=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        GError *error=NULL;
        if (select_iq_file(a,path,&error)) { file_selection_sync(a); status(a,"IQ file loaded. Check Mode; click Play file to play in real time."); }
        else {
            GtkWidget *message=gtk_message_dialog_new(GTK_WINDOW(a->config_dialog),GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR,GTK_BUTTONS_CLOSE,"%s",error->message);
            gtk_dialog_run(GTK_DIALOG(message)); gtk_widget_destroy(message); g_clear_error(&error);
        }
        g_free(path);
    }
    gtk_widget_destroy(chooser);
}

static GtkWidget *entry_field(GtkWidget *grid, const char *label, int row, App *a,
                              const char *section, const char *key) {
    GtkWidget *entry = gtk_entry_new(), *caption = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(caption), 0);
    gtk_grid_attach(GTK_GRID(grid), caption, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
    gtk_widget_set_hexpand(entry, TRUE);
    char *value = setting(a, section, key);
    gtk_entry_set_text(GTK_ENTRY(entry), value); g_free(value);
    return entry;
}

static void hardware_entry_changed(GtkWidget *widget, gpointer data) {
    App *a = data;
    const char *keys[] = {"args", "images", "antenna"};
    const char *net_keys[] = {"ip", "port", "bind"};
    for (int source = 1; source < SOURCE_FILE; ++source)
        for (int field = 0; field < 3; ++field)
            if (widget == a->hardware_entries[source][field])
                g_key_file_set_string(a->config, sources[source], source == SOURCE_AFEDRI ? net_keys[field] : keys[field],
                                     gtk_entry_get_text(GTK_ENTRY(widget)));
    if (!a->running) refresh_preview(a);
}

static gboolean apply_hardware(App *a) {
    if (a->running) return TRUE;
    if (a->source == SOURCE_AFEDRI) {
        struct in_addr addr;
        const char *net_ip = gtk_entry_get_text(GTK_ENTRY(a->hardware_entries[SOURCE_AFEDRI][0]));
        const char *port = gtk_entry_get_text(GTK_ENTRY(a->hardware_entries[SOURCE_AFEDRI][1]));
        const char *bind = gtk_entry_get_text(GTK_ENTRY(a->hardware_entries[SOURCE_AFEDRI][2]));
        char *end; errno = 0;
        long value = strtol(port, &end, 10);
        if (inet_pton(AF_INET, net_ip, &addr) != 1 || inet_pton(AF_INET, bind, &addr) != 1 ||
            errno || end == port || *end || value < 1 || value > 65535) {
            GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(a->config_dialog), GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "Enter valid Afedri IPv4 addresses and a TCP / UDP port from 1 to 65535.");
            gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog); return FALSE;
        }
    }
    const char *ip = gtk_entry_get_text(GTK_ENTRY(a->ip_box));
    struct in_addr address;
    if (inet_pton(AF_INET, ip, &address) != 1) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(a->config_dialog), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "Enter a valid IPv4 address for Linrad.");
        gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
        gtk_widget_grab_focus(a->ip_box); return FALSE;
    }
    GtkWidget *fields[] = {a->rate_box, a->port_box, a->header_box, a->center_box,
                           a->frequency_box, a->offset_box, a->receiver_offset_box, a->step_box, a->file_header, a->file_center};
    for (unsigned i=0; i<G_N_ELEMENTS(fields); ++i)
        if (gtk_widget_is_sensitive(fields[i])) gtk_spin_button_update(GTK_SPIN_BUTTON(fields[i]));
    g_key_file_set_string(a->config, "linrad", "ip", ip);
    g_key_file_set_integer(a->config, "linrad", "port", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->port_box)));
    g_key_file_set_integer(a->config, "linrad", "header_bytes", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->header_box)));
    g_key_file_set_double(a->config, "linrad", "frequency", gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->center_box))*1e6);
    refresh_preview(a); return TRUE;
}

static void config_response(GtkDialog *dialog, gint response, gpointer data) {
    (void)response;
    if (apply_hardware(data)) gtk_widget_hide(GTK_WIDGET(dialog));
}

static gboolean config_close(GtkWidget *widget, GdkEvent *event, gpointer data) {
    (void)event;
    config_response(GTK_DIALOG(widget), GTK_RESPONSE_CLOSE, data); return TRUE;
}

static void show_config(GtkWidget *widget, gpointer data) {
    (void)widget; App *a = data;
    gtk_widget_show_all(a->config_dialog);
    gtk_widget_set_visible(a->rate_box, a->source != 3 && a->source != SOURCE_RTLSDR && a->source != SOURCE_FCDPP && a->source != SOURCE_AIRSPYR2);
    gtk_widget_set_visible(a->sdriq_rate_box, a->source == 3 || a->source == SOURCE_RTLSDR || a->source == SOURCE_FCDPP || a->source == SOURCE_AIRSPYR2);
    gtk_window_present(GTK_WINDOW(a->config_dialog));
}

static GtkWidget *about_label(GtkWidget *box, const char *text, const char *style) {
    GtkWidget *label=gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label),0);
    gtk_label_set_line_wrap(GTK_LABEL(label),TRUE);
    /* A minimum paragraph width prevents GTK from requesting the height
     * needed to wrap the entire paragraph into a narrow column. */
    if (!style || !strcmp(style,"about-meta")) gtk_label_set_width_chars(GTK_LABEL(label),76);
    gtk_label_set_max_width_chars(GTK_LABEL(label),76);
    gtk_label_set_selectable(GTK_LABEL(label),TRUE);
    if (style) gtk_style_context_add_class(gtk_widget_get_style_context(label),style);
    gtk_box_pack_start(GTK_BOX(box),label,FALSE,FALSE,0);
    return label;
}

static void about_link(GtkWidget *box, const char *title, const char *uri) {
    GtkWidget *link=gtk_link_button_new_with_label(uri,title);
    gtk_widget_set_halign(link,GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box),link,FALSE,FALSE,0);
}

static void show_about(GtkWidget *widget, gpointer data) {
    (void)widget; App *a = data;
    GtkWidget *dialog=gtk_dialog_new_with_buttons("About · Linrad DVB",GTK_WINDOW(a->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,"Close",GTK_RESPONSE_CLOSE,NULL);
    /* Keep the longer command/credits reference usable on small screens. */
    GdkMonitor *monitor=gdk_display_get_monitor_at_window(gtk_widget_get_display(a->window),gtk_widget_get_window(a->window));
    GdkRectangle work={0,0,1024,768};
    if (monitor) gdk_monitor_get_workarea(monitor,&work);
    /* Headless Broadway displays can report an empty work area. */
    if (work.width<=0) work.width=1024;
    if (work.height<=0) work.height=768;
    /* Leave room for the title bar, borders and client-side shadows. */
    gtk_window_set_default_size(GTK_WINDOW(dialog),MIN(760,work.width*4/5),MIN(720,work.height*4/5));
    GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),scroll,TRUE,TRUE,0);
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);
    gtk_container_set_border_width(GTK_CONTAINER(box),12);
    gtk_container_add(GTK_CONTAINER(scroll),box);
    about_label(box,"linrad_dvb","about-title");
    about_label(box,"linrad_dvb is an application for amateur radio operators receiving digital "
        "amateur television under weak signal conditions.",NULL);
    about_label(box,"TSscatterFix is a utility that uses machine learning to reconstruct recurring "
        "elements in static test patterns from fragments of the received "
        "MPEG-TS stream. It is designed specifically for (D)ATV contests.",NULL);
    about_label(box,"Created by Rob Hardenberg, PE1ITR.",NULL);
    /* Compiler timestamps identify this executable, not the date it is opened. */
    about_label(box,"Build date: " __DATE__ " · " __TIME__,"about-meta");
    about_label(box,"Receiver controls","about-heading");
    about_label(box,"Power: start / stop reception. Input: Linrad, SDR hardware or IQ-file playback. "
        "Mode: DVB-S/S2/T, symbol rate or bandwidth, FEC, constellation and guard interval. "
        "For DVB-S2, choose Soft-decision LDPC or Bit-flipping, 1–8 LDPC workers, "
        "and automatic acquisition/processing based on symbol rate: up to 333 ksym/s, "
        "continuous search and low latency; above that, normal search and throughput. "
        "Manual overrides are available. W saves these settings.",NULL);
    about_label(box,"Renderer: embedded video, UDP, external player or MPEG-TS file. "
        "Spectrum controls: IQ filter, IQ recording, TSscatterFix, USB / FM / FM-DATA, "
        "Service info / EPG / Teletext. TCI: monitor audio and control connection.",NULL);
    about_label(box,"Keyboard shortcuts","about-heading");
    GtkWidget *grid=gtk_grid_new();
    gtk_widget_set_name(grid,"about-shortcuts");
    gtk_box_pack_start(GTK_BOX(box),grid,FALSE,FALSE,0);
    const char *shortcuts[][2]={
        {"Key","Description"},
        {"Alt+I / Alt+M / Alt+R / Alt+T / Alt+A","Open Input / Mode / Renderer / TCI / About"},
        {"R","Reset TSscatterFix reconstruction"},
        {"M","Mute / unmute speakers (DVB and USB / FM / FM-DATA); TCI audio continues"},
        {"U","USB receiver (3 kHz), speaker and TCI on/off"},
        {"F","FM receiver (12.5 kHz), speaker and TCI on/off"},
        {"D","FM-DATA receiver (20 kHz), flat audio to speaker and TCI on/off"},
        {"Page Up / Page Down","Tune monitor down / up: USB 100 Hz, FM / FM-DATA 2.5 kHz"},
        {"Click spectrum","Tune the active USB / FM / FM-DATA monitor at the clicked frequency"},
        {"← / →","Decrease / increase receive frequency"},
        {"↑ / ↓","Increase / decrease gain"},
        {"[ / ]","Narrow / widen the IQ bandpass filter"},
        {"W","Save settings, including spectrum and waterfall"},
        {"P","Save the entire main window as a PNG screenshot"}
    };
    for (unsigned row=0;row<G_N_ELEMENTS(shortcuts);++row) {
        for (int column=0;column<2;++column) {
            GtkWidget *cell=gtk_label_new(shortcuts[row][column]);
            gtk_label_set_xalign(GTK_LABEL(cell),column ? 0 : .5);
            gtk_label_set_line_wrap(GTK_LABEL(cell),TRUE);
            gtk_style_context_add_class(gtk_widget_get_style_context(cell),
                row==0 ? "about-table-heading" : column==0 ? "about-key" : "about-description");
            if (column==0) gtk_label_set_width_chars(GTK_LABEL(cell),14);
            else {
                gtk_widget_set_hexpand(cell,TRUE);
                gtk_label_set_max_width_chars(GTK_LABEL(cell),48);
            }
            gtk_grid_attach(GTK_GRID(grid),cell,column,row,1,1);
        }
    }
    about_label(box,"Shortcuts work in the main window, including lowercase keys and during reception. "
        "In settings dialogs, keys remain available for text entry.","about-meta");
    about_label(box,"When using Linrad, adjust frequency and gain in Linrad itself.","about-meta");
    about_label(box,"Screenshots are saved beside the INI file as "
        "linrad-dvb-[UTC timestamp]-[mode]-[SR or BW]-[frequency in whole kHz].png. "
        "SR is in symbols/s; BW is in Hz. "
        "For Linrad input, the frequency is the value entered in Input.","about-meta");
    about_label(box,"Command-line reference","about-heading");
    about_label(box,"./build/linrad_dvb --help\n"
        "./build/linrad_dvb --source linrad --standard DVB-S2 --sr 125000 --fec 1/2 --print-pipeline\n"
        "./build/linrad_dvb --config linrad-dvb.ini --run\n"
        "./build/linrad_dvb --decoder-worker=leansdr --help\n"
        "./build/linrad_dvb --decoder-worker=dvbt --help\n"
        "./build/ldpc_tool --help\n"
        "./build/ldpc_tool --version",NULL);
    about_label(box,"Use --print-pipeline to inspect the configured commands without starting reception. "
        "--run starts reception without the GUI; Ctrl+C stops it. LDPC preferences live in "
        "Mode / [leandvb] in the INI file. Decoder-worker options are for direct IQ-to-TS use.","about-meta");
    about_label(box,"Acknowledgements","about-heading");
    about_label(box,"LeanSDR / LeanDVB: pabr@pabr.org, DVB-S/S2 demodulation (GPL-3.0-or-later). "
        "LDPC: Ahmet Inan (xdsopl), with the external decoder adaptation by pabr@pabr.org (0BSD). "
        "DVB-S2 acquisition corrections: Ondřej OK1CDJ, LeanDVB issue #41. "
        "rbdvbt and TSscatterFix: Rob Hardenberg, PE1ITR.",NULL);
    about_link(box,"LeanSDR / LeanDVB · pabr","https://github.com/pabr/leansdr");
    about_link(box,"LDPC · Ahmet Inan (xdsopl)","https://github.com/xdsopl/LDPC");
    about_link(box,"LDPC helper adaptation · pabr","https://github.com/pabr/xdsopl-LDPC-pabr");
    about_link(box,"Acquisition patch · Ondřej OK1CDJ (issue #41)","https://github.com/pabr/leansdr/issues/41");
    about_label(box,"Also built on Linrad, GTK / GLib, libVLC, FFmpeg, FFTW, libzvbi, "
        "ALSA, libsamplerate, SoapySDR and UHD. Thanks to their authors and contributors.",NULL);
    about_label(box,"Source revisions, local changes and license texts are included under third_party/. "
        "make package copies the bundled decoder/recovery licenses into build/.","about-meta");
    /* Focusing a credit link would scroll past the introduction on opening. */
    gtk_widget_grab_focus(gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),GTK_RESPONSE_CLOSE));
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
}

static void create_config(App *a) {
    a->config_dialog = gtk_dialog_new_with_buttons("SDR · input and hardware", GTK_WINDOW(a->window),
        GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, "Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(a->config_dialog), 620, -1);
    a->hardware = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(a->hardware), 12);
    gtk_grid_set_row_spacing(GTK_GRID(a->hardware), 8);
    gtk_grid_set_column_spacing(GTK_GRID(a->hardware), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(a->config_dialog))), a->hardware, TRUE, TRUE, 0);
    a->source_box = combo(a->hardware, "Input / SDR", 0, source_labels, G_N_ELEMENTS(source_labels), a->source);
    a->rate_box = spin(a->hardware, "Sample rate (samples/s)", 1, 1, 100000000, 100000);
    a->sdriq_rate_box = gtk_combo_box_text_new();
    gtk_grid_attach(GTK_GRID(a->hardware), a->sdriq_rate_box, 1, 1, 1, 1);
    for (unsigned i = 0; i < G_N_ELEMENTS(sdriq_rates); ++i) {
        char *rate_id = g_strdup_printf("%d", sdriq_rates[i]);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(a->sdriq_rate_box), rate_id, rate_id); g_free(rate_id);
    }
    gtk_widget_set_no_show_all(a->rate_box, TRUE);
    gtk_widget_set_no_show_all(a->sdriq_rate_box, TRUE);
    a->format_box = combo(a->hardware, "IQ format", 2, (const char *[]) {"s16", "u8"}, 2, 0);
    a->hardware_stack = gtk_stack_new();
    gtk_grid_attach(GTK_GRID(a->hardware), a->hardware_stack, 0, 3, 2, 1);
    for (int source=0; source<(int)G_N_ELEMENTS(sources); ++source) {
        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 8); gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
        gtk_stack_add_named(GTK_STACK(a->hardware_stack), grid, sources[source]);
        if (!source) {
            a->ip_box = entry_field(grid, "UDP bind address", 0, a, "linrad", "ip");
            a->port_box = spin(grid, "UDP port", 1, 1, 65535, 1);
            a->header_box = spin(grid, "Header (bytes)", 2, 0, 65534, 1);
            a->center_box = spin(grid, "Receive frequency (MHz)", 3, 0, 100000, 1);
            gtk_spin_button_set_digits(GTK_SPIN_BUTTON(a->center_box), 6);
            gtk_widget_set_tooltip_text(a->center_box, "Set Linrad's center frequency here for the MHz scale. This does not tune Linrad.");
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->port_box), integer_setting(a, "linrad", "port", 1, 65535));
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->header_box), integer_setting(a, "linrad", "header_bytes", 0, 65534));
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->center_box), g_key_file_get_double(a->config, "linrad", "frequency", NULL)/1e6);
        } else if (source==SOURCE_FILE) {
            a->file_path=entry_field(grid,"IQ file",0,a,"file","path");
            gtk_editable_set_editable(GTK_EDITABLE(a->file_path),FALSE);
            GtkWidget *browse=gtk_button_new_with_label("Browse…");
            gtk_grid_attach(GTK_GRID(grid),browse,2,0,1,1);
            g_signal_connect(browse,"clicked",G_CALLBACK(choose_iq_file),a);
            a->file_format=combo(grid,"File format",1,file_formats,G_N_ELEMENTS(file_formats),0);
            a->file_header=spin(grid,"Skip header (bytes)",2,0,100000000,1);
            a->file_center=spin(grid,"Receive frequency (MHz)",3,0,1000000,1);
            gtk_spin_button_set_digits(GTK_SPIN_BUTTON(a->file_center),6);
            a->file_details=gtk_label_new("");
            gtk_label_set_xalign(GTK_LABEL(a->file_details),0);
            gtk_label_set_line_wrap(GTK_LABEL(a->file_details),TRUE);
            gtk_label_set_line_wrap_mode(GTK_LABEL(a->file_details),PANGO_WRAP_WORD_CHAR);
            gtk_label_set_max_width_chars(GTK_LABEL(a->file_details),65);
            gtk_grid_attach(GTK_GRID(grid),a->file_details,0,4,3,1);
            g_signal_connect(a->file_format,"changed",G_CALLBACK(file_options_changed),a);
            g_signal_connect(a->file_header,"value-changed",G_CALLBACK(file_options_changed),a);
            g_signal_connect(a->file_center,"value-changed",G_CALLBACK(file_options_changed),a);
        } else {
            const char *keys[] = {"args", "images", "antenna"};
            const char *labels[] = {"Device arguments", "UHD images (B210)", "Antenna"};
            const char *net_keys[] = {"ip", "port", "bind"};
            const char *net_labels[] = {"Afedri IP address", "TCP / UDP port", "UDP bind address"};
            for (int field=0; field<((source == SOURCE_FCDPP || source == SOURCE_AIRSPYHF || source == SOURCE_AIRSPYR2 || rx_rsp(sources[source])) ? 1 : 3); ++field) {
                if (source == SOURCE_LIME && field == 1) continue;
                GtkWidget *entry = entry_field(grid, source == SOURCE_AFEDRI ? net_labels[field] : labels[field],
                    field, a, sources[source], source == SOURCE_AFEDRI ? net_keys[field] : keys[field]);
                if (source == SOURCE_LIME) gtk_widget_set_tooltip_text(entry, field == 0 ? "Empty for a single device, or serial=SERIAL to select a Mini. Native LimeSuite RX channel 0." : "AUTO switches LNAW below 2 GHz / LNAH from 2 GHz; or select LNAW or LNAH explicitly.");
                if (source == SOURCE_FCDPP) gtk_widget_set_tooltip_text(entry, "Optional: hid=/dev/hidrawN,lna=0|1,mixer=0|1. Gain arrows set IF gain (0..59 dB). If the spectrum collapses at high IF gain, reduce IF gain or try lna=1,mixer=0 while stopped. The usable gain depends on the mixer setting and input signal.");
                if (source == SOURCE_AIRSPYR2) gtk_widget_set_tooltip_text(entry,"SoapyAirspy + libairspy. Optional serial=HEX selects a device. 24..1700 MHz; 10 MS/s or experimental 2.5 MS/s. Gain setting 0..45 distributes LNA/MIX/VGA steps; not calibrated dB. Bias tee off by default.");
                if (source == SOURCE_AIRSPYHF) gtk_widget_set_tooltip_text(entry,"SoapyAirspyHF: driver=airspyhf,serial=HEX (serial optional). Connect the VHF input: 64..260 MHz. VHF gain is automatic; sample rate is checked against the device at start.");
                if (rx_rsp(sources[source])) gtk_widget_set_tooltip_text(entry,"SoapySDRPlay3 and SDRplay API v3 required. Optional serial=SERIAL selects a device. The selected RSP model is checked at start. Gain arrows adjust IF reduction (20..59 dB); higher means less gain. RF reduction state is 0.");
                a->hardware_entries[source][field] = entry;
                g_signal_connect(entry, "changed", G_CALLBACK(hardware_entry_changed), a);
            }
            if (source == SOURCE_FCDPP) {
                GtkWidget *note = gtk_label_new("Fixed 192 kS/s · IF gain 0–59 dB\nUse narrowband DVB settings, e.g. DVB-T 40 kHz.\nTuning: 0.15–240 MHz and 420–1900 MHz.\nSpectrum disappears at high gain? Reduce IF gain\nor try lna=1,mixer=0 in Device arguments.");
                gtk_label_set_xalign(GTK_LABEL(note), 0);
                gtk_grid_attach(GTK_GRID(grid), note, 0, 1, 2, 1);
            }
        }
    }
    a->frequency_box = spin(a->hardware, "SDR / IF (kHz)", 4, 1000, 6000000.0, 1);
    a->offset_box = spin(a->hardware, "LO offset (kHz)", 5, -100000000, 100000000, 1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(a->offset_box), 3);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->offset_box), 0);
    gtk_widget_set_tooltip_text(a->offset_box, "Display = SDR frequency + LO offset. For 436 MHz via 28 MHz: 408000 kHz.");
    a->receiver_offset_box=spin(a->hardware,"DVB receiver offset (kHz)",6,-50000,50000,1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(a->receiver_offset_box),3);
    gtk_widget_set_tooltip_text(a->receiver_offset_box,"Receive = SDR + converter LO + receiver offset. Positive selects above the spectrum center; 0 bypasses digital mixing. Change while stopped.");
    a->step_box = spin(a->hardware, "Tuning step (Hz)", 7, 1, 10000000, 100);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->step_box), 1000);
    a->frequency_calculation = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(a->frequency_calculation), 0);
    gtk_label_set_selectable(GTK_LABEL(a->frequency_calculation), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(a->frequency_calculation), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(a->frequency_calculation), 70);
    gtk_grid_attach(GTK_GRID(a->hardware), a->frequency_calculation, 0, 8, 2, 1);
    GtkWidget *hint = gtk_label_new("Changes apply to this session. Press W in the main window to save them.");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_grid_attach(GTK_GRID(a->hardware), hint, 0, 9, 2, 1);
    g_signal_connect(a->config_dialog, "response", G_CALLBACK(config_response), a);
    g_signal_connect(a->config_dialog, "delete-event", G_CALLBACK(config_close), a);
}

static GtkWidget *settings_dialog(App *a, const char *title, GtkWidget **grid) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(a->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Close", GTK_RESPONSE_CLOSE, NULL);
    *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(*grid), 8); gtk_grid_set_column_spacing(GTK_GRID(*grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(*grid), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), *grid, TRUE, TRUE, 0);
    return dialog;
}

static void hide_dialog(GtkDialog *dialog, gint response, gpointer data) {
    (void)response; (void)data; gtk_widget_hide(GTK_WIDGET(dialog));
}

static gboolean hide_dialog_close(GtkWidget *widget, GdkEvent *event, gpointer data) {
    (void)event; (void)data; gtk_widget_hide(widget); return TRUE;
}

static void show_mode(GtkWidget *widget, gpointer data) {
    (void)widget; App *a=data; gtk_widget_show_all(a->mode_dialog);
    gtk_window_present(GTK_WINDOW(a->mode_dialog));
}

static void show_renderer(GtkWidget *widget, gpointer data) {
    (void)widget; App *a=data; gtk_widget_show_all(a->renderer_dialog);
    gtk_window_present(GTK_WINDOW(a->renderer_dialog));
}

static void show_tci(GtkWidget *widget, gpointer data) {
    (void)widget; App *a=data; gtk_widget_show_all(a->tci_dialog);
    gtk_window_present(GTK_WINDOW(a->tci_dialog));
}

static void pcm_udp_destination_changed(GtkWidget *widget, gpointer data) {
    (void)widget; App *a=data;
    g_key_file_set_string(a->config,"usb","pcm_udp_host",gtk_entry_get_text(GTK_ENTRY(a->pcm_udp_host)));
    g_key_file_set_integer(a->config,"usb","pcm_udp_port",gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->pcm_udp_port)));
}
static void pcm_udp_toggled(GtkToggleButton *button, gpointer data) {
    App *a=data;
    gboolean enabled=gtk_toggle_button_get_active(button);
    gtk_spin_button_update(GTK_SPIN_BUTTON(a->pcm_udp_port));
    const char *host=gtk_entry_get_text(GTK_ENTRY(a->pcm_udp_host));
    unsigned port=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->pcm_udp_port));
    GError *error=NULL;
    if (!usb_receiver_set_pcm_udp(a->usb_receiver,enabled,host,port,&error)) {
        enabled=usb_receiver_pcm_udp_enabled(a->usb_receiver);
        g_signal_handlers_block_by_func(button,pcm_udp_toggled,a);
        gtk_toggle_button_set_active(button,enabled);
        g_signal_handlers_unblock_by_func(button,pcm_udp_toggled,a);
        gtk_label_set_text(GTK_LABEL(a->usb_status),error->message);
        status(a,error->message); g_clear_error(&error);
    }
    g_key_file_set_boolean(a->config,"usb","pcm_udp_enabled",enabled);
    pcm_udp_destination_changed(NULL,a);
    gtk_widget_set_sensitive(a->pcm_udp_host,!enabled);
    gtk_widget_set_sensitive(a->pcm_udp_port,!enabled);
}
static void create_tci(App *a, const char *error) {
    GtkWidget *grid;
    a->tci_dialog=settings_dialog(a,"TCI · USB / FM / FM-DATA receiver",&grid);
    a->usb_port=spin(grid,"TCI port",0,1,65535,1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->usb_port),integer_setting(a,"usb","tci_port",1,65535));
    GtkWidget *hint=gtk_label_new("Switch USB / FM / FM-DATA off to change the port. Press W in the main window to save.\n"
        "USB (U), FM (F) or FM-DATA (D) enables the speaker and TCI. Click the spectrum to tune.\n"
        "Page Up / Down: lower / higher frequency; USB 100 Hz, FM / FM-DATA 2.5 kHz.");
    gtk_label_set_line_wrap(GTK_LABEL(hint),TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(hint),60);
    gtk_label_set_xalign(GTK_LABEL(hint),0);
    gtk_grid_attach(GTK_GRID(grid),hint,0,1,2,1);
    a->usb_status=gtk_label_new(error ? error : "USB / FM / FM-DATA off");
    gtk_label_set_line_wrap(GTK_LABEL(a->usb_status),TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(a->usb_status),60);
    gtk_label_set_xalign(GTK_LABEL(a->usb_status),0);
    gtk_grid_attach(GTK_GRID(grid),a->usb_status,0,2,2,1);
    a->pcm_udp_enabled=gtk_check_button_new_with_label("Send PCM audio via UDP");
    gtk_grid_attach(GTK_GRID(grid),a->pcm_udp_enabled,0,3,2,1);
    a->pcm_udp_host=entry_field(grid,"PCM destination IPv4",4,a,"usb","pcm_udp_host");
    a->pcm_udp_port=spin(grid,"PCM UDP port",5,1,65535,1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->pcm_udp_port),integer_setting(a,"usb","pcm_udp_port",1,65535));
    GtkWidget *pcm_hint=gtk_label_new("48 kHz · mono · signed 16-bit little-endian · raw UDP audio\n"
        "Dire Wolf: FM for 1200 AFSK; FM-DATA for 9600 G3RUH.\n"
        "M mutes speakers only; TCI and PCM audio continue.\n"
        "PCM follows the active USB / FM / FM-DATA receiver.\n"
        "Switch PCM off to edit its destination. Press W in the main window to save.");
    gtk_label_set_line_wrap(GTK_LABEL(pcm_hint),TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(pcm_hint),60);
    gtk_label_set_xalign(GTK_LABEL(pcm_hint),0);
    gtk_grid_attach(GTK_GRID(grid),pcm_hint,0,6,2,1);
    g_signal_connect(a->pcm_udp_host,"changed",G_CALLBACK(pcm_udp_destination_changed),a);
    g_signal_connect(a->pcm_udp_port,"value-changed",G_CALLBACK(pcm_udp_destination_changed),a);
    g_signal_connect(a->pcm_udp_enabled,"toggled",G_CALLBACK(pcm_udp_toggled),a);
    gtk_widget_set_sensitive(a->pcm_udp_enabled,a->usb_receiver!=NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->pcm_udp_enabled),
        g_key_file_get_boolean(a->config,"usb","pcm_udp_enabled",NULL));
    g_signal_connect(a->usb_port,"value-changed",G_CALLBACK(usb_port_changed),a);
    g_signal_connect(a->tci_dialog,"response",G_CALLBACK(hide_dialog),a);
    g_signal_connect(a->tci_dialog,"delete-event",G_CALLBACK(hide_dialog_close),a);
}

static void renderer_selection_changed(GtkWidget *widget, gpointer data) {
    App *a=data;
    selection_changed(widget, data);
    gtk_widget_set_sensitive(a->renderer_program, a->renderer == RENDER_CUSTOM);
    gtk_widget_set_sensitive(a->renderer_args, a->renderer == RENDER_CUSTOM);
    gtk_widget_set_sensitive(a->output_file, a->renderer == RENDER_FILE);
    gtk_widget_set_sensitive(a->udp_enabled, a->renderer == RENDER_EMBEDDED);
    gtk_widget_set_sensitive(a->udp_host, internal_player(a));
    gtk_widget_set_sensitive(a->udp_port, internal_player(a));
    scatter_refresh(a);
}

static gboolean apply_renderer(App *a) {
    if (a->running) return TRUE;
    const char *program = gtk_entry_get_text(GTK_ENTRY(a->renderer_program));
    const char *args = gtk_entry_get_text(GTK_ENTRY(a->renderer_args));
    const char *file = gtk_entry_get_text(GTK_ENTRY(a->output_file));
    GError *error=NULL; char **argv=NULL; int argc;
    if (a->renderer == RENDER_CUSTOM) {
        if (!*program) { gtk_label_set_text(GTK_LABEL(a->renderer_error), "Enter a program name or path."); return FALSE; }
        if (*args && !g_shell_parse_argv(args, &argc, &argv, &error)) {
            gtk_label_set_text(GTK_LABEL(a->renderer_error), error->message); g_clear_error(&error); return FALSE;
        }
        g_strfreev(argv);
    }
    if (a->renderer == RENDER_FILE && !*file) {
        gtk_label_set_text(GTK_LABEL(a->renderer_error), "Enter a filename for the MPEG-TS output."); return FALSE;
    }
    if (internal_player(a)) {
        gboolean enabled = a->renderer == RENDER_UDP || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->udp_enabled));
        const char *host = gtk_entry_get_text(GTK_ENTRY(a->udp_host));
        gtk_spin_button_update(GTK_SPIN_BUTTON(a->udp_port));
        int port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->udp_port));
        struct in_addr destination;
        if (inet_pton(AF_INET, host, &destination) != 1 ||
            destination.s_addr == INADDR_ANY || destination.s_addr == INADDR_BROADCAST) {
            gtk_label_set_text(GTK_LABEL(a->renderer_error), "Enter a destination IPv4 address, e.g. 127.0.0.1."); return FALSE;
        }
        if (a->renderer == RENDER_EMBEDDED) g_key_file_set_boolean(a->config, "renderer", "udp_enabled", enabled);
        g_key_file_set_string(a->config, "renderer", "udp_host", host);
        g_key_file_set_integer(a->config, "renderer", "udp_port", port);
    }
    /* Only store active fields, so an unfinished inactive command cannot break a later preview. */
    if (a->renderer == RENDER_CUSTOM) {
        g_key_file_set_string(a->config, "paths", "custom_renderer", program);
        g_key_file_set_string(a->config, "renderer", "args", args);
    }
    if (a->renderer == RENDER_FILE) g_key_file_set_string(a->config, "renderer", "output_file", file);
    gtk_label_set_text(GTK_LABEL(a->renderer_error), "");
    refresh_preview(a); return TRUE;
}

static void renderer_response(GtkDialog *dialog, gint response, gpointer data) {
    (void)response; if (apply_renderer(data)) gtk_widget_hide(GTK_WIDGET(dialog));
}

static gboolean renderer_close(GtkWidget *widget, GdkEvent *event, gpointer data) {
    (void)event; renderer_response(GTK_DIALOG(widget), GTK_RESPONSE_CLOSE, data); return TRUE;
}

static void ldpc_settings_changed(GtkWidget *widget, gpointer data) {
    (void)widget; App *a=data;
    int mode=gtk_combo_box_get_active(GTK_COMBO_BOX(a->ldpc_mode_box));
    g_key_file_set_string(a->config,"leandvb","ldpc_mode",mode ? "bitflip" : "soft");
    g_key_file_set_integer(a->config,"leandvb","helpers",gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->ldpc_workers_box)));
    g_key_file_set_string(a->config,"leandvb","latency",s2_latency_modes[gtk_combo_box_get_active(GTK_COMBO_BOX(a->ldpc_latency_box))]);
    g_key_file_set_string(a->config,"leandvb","acquisition",s2_acquisition_modes[gtk_combo_box_get_active(GTK_COMBO_BOX(a->s2_acquisition_box))]);
    gtk_widget_set_sensitive(a->ldpc_workers_box,a->standard==1 && !mode);
    gtk_widget_set_sensitive(a->ldpc_latency_box,a->standard==1 && !mode);
    if (!a->running) refresh_preview(a);
}

static void create_mode_renderer(App *a) {
    a->mode_dialog = settings_dialog(a, "Mode · DVB settings", &a->controls);
    a->standard_box = combo(a->controls, "Mode", 0, standards, 3, a->standard);
    a->sr_box = combo(a->controls, "Symbol rate (sym/s)", 1, NULL, 0, -1);
    GList *rate_cells=gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(a->sr_box));
    for (GList *cell=rate_cells;cell;cell=cell->next)
        gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(a->sr_box),cell->data,rate_cell_data,a,NULL);
    g_list_free(rate_cells);
    a->sr_label = gtk_grid_get_child_at(GTK_GRID(a->controls), 0, 1);
    a->fec_box = combo(a->controls, "FEC", 2, NULL, 0, -1);
    a->gi_box = combo(a->controls, "Guard interval", 3, guards, 4, a->gi);
    a->constellation_box = combo(a->controls, "Constellation", 4, constellations, 2, a->constellation);
    char *ldpc_mode=setting(a,"leandvb","ldpc_mode");
    a->ldpc_mode_box=combo(a->controls,"DVB-S2 error correction",5,(const char *[]){"Soft-decision LDPC","Bit-flipping"},2,!strcmp(ldpc_mode,"bitflip"));
    a->ldpc_workers_box=spin(a->controls,"LDPC workers",6,1,8,1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->ldpc_workers_box),integer_setting(a,"leandvb","helpers",1,8));
    a->ldpc_latency_box=combo(a->controls,"LDPC processing",7,(const char *[]){"Automatic (symbol rate)","Low latency","Throughput"},3,
        s2_choice(a,"latency",s2_latency_modes,G_N_ELEMENTS(s2_latency_modes)));
    a->s2_acquisition_box=combo(a->controls,"DVB-S2 acquisition",8,(const char *[]){"Automatic (symbol rate)","Continuous search","Normal search"},3,
        s2_choice(a,"acquisition",s2_acquisition_modes,G_N_ELEMENTS(s2_acquisition_modes)));
    GtkWidget *auto_hint=gtk_label_new("Automatic: up to 333 ksym/s, continuous search + low latency.\n"
        "Above 333 ksym/s, normal search + throughput.");
    gtk_label_set_xalign(GTK_LABEL(auto_hint),0);
    gtk_grid_attach(GTK_GRID(a->controls),auto_hint,0,9,2,1);
    g_free(ldpc_mode);
    g_signal_connect(a->ldpc_mode_box,"changed",G_CALLBACK(ldpc_settings_changed),a);
    g_signal_connect(a->ldpc_workers_box,"value-changed",G_CALLBACK(ldpc_settings_changed),a);
    g_signal_connect(a->ldpc_latency_box,"changed",G_CALLBACK(ldpc_settings_changed),a);
    g_signal_connect(a->s2_acquisition_box,"changed",G_CALLBACK(ldpc_settings_changed),a);
    a->renderer_dialog = settings_dialog(a, "Renderer · MPEG-TS output", &a->renderer_panel);
    a->renderer_box = combo(a->renderer_panel, "Output", 0,
        (const char *[]) {"VLC (cvlc)", "VLC with controls", "FFplay", "Custom program", "MPEG-TS file", "Embedded player (right panel)", "UDP stream (external player)"},
        G_N_ELEMENTS(renderers), a->renderer);
    a->renderer_program = entry_field(a->renderer_panel, "Program / path", 1, a, "paths", "custom_renderer");
    a->renderer_args = entry_field(a->renderer_panel, "Arguments", 2, a, "renderer", "args");
    a->output_file = entry_field(a->renderer_panel, "MPEG-TS file", 3, a, "renderer", "output_file");
    a->udp_enabled = gtk_check_button_new_with_label("Also send video/audio via UDP");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->udp_enabled),
        g_key_file_get_boolean(a->config, "renderer", "udp_enabled", NULL));
    gtk_grid_attach(GTK_GRID(a->renderer_panel), a->udp_enabled, 0, 4, 2, 1);
    a->udp_host = entry_field(a->renderer_panel, "UDP destination IPv4", 5, a, "renderer", "udp_host");
    a->udp_port = spin(a->renderer_panel, "UDP destination port", 6, 1, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->udp_port), integer_setting(a, "renderer", "udp_port", 1, 65535));
    gtk_widget_set_sensitive(a->udp_enabled, a->renderer == RENDER_EMBEDDED);
    gtk_widget_set_sensitive(a->udp_host, internal_player(a));
    gtk_widget_set_sensitive(a->udp_port, internal_player(a));
    GtkWidget *hint = gtk_label_new("Embedded player: fixed panel; video scales to fit without changing its shape.\n"
        "UDP works alongside the embedded player. In VLC, open udp://@:1234 (default port).\n"
        "Custom program: MPEG-TS on stdin; {input} becomes -.\n"
        "TSscatterFix receives a parallel copy; all outputs keep the original stream.\n"
        "Relative paths start in the INI directory. Existing files will not be overwritten.");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(hint), 64);
    gtk_grid_attach(GTK_GRID(a->renderer_panel), hint, 0, 7, 2, 1);
    a->renderer_error = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(a->renderer_error), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(a->renderer_error), 64);
    gtk_grid_attach(GTK_GRID(a->renderer_panel), a->renderer_error, 0, 8, 2, 1);
    gtk_widget_set_sensitive(a->renderer_program, a->renderer == RENDER_CUSTOM);
    gtk_widget_set_sensitive(a->renderer_args, a->renderer == RENDER_CUSTOM);
    gtk_widget_set_sensitive(a->output_file, a->renderer == RENDER_FILE);
    scatter_refresh(a);
    g_signal_connect(a->mode_dialog, "response", G_CALLBACK(hide_dialog), a);
    g_signal_connect(a->mode_dialog, "delete-event", G_CALLBACK(hide_dialog_close), a);
    g_signal_connect(a->renderer_dialog, "response", G_CALLBACK(renderer_response), a);
    g_signal_connect(a->renderer_dialog, "delete-event", G_CALLBACK(renderer_close), a);
}

static void gui(App *a) {
    source_defaults(a);
    if (!g_key_file_has_key(a->config, "session", "renderer", NULL)) a->renderer = RENDER_EMBEDDED;
    /* Keep GTK's built-in dialogs and errors in the same language as the UI. */
    setlocale(LC_ALL, "");
    setlocale(LC_MESSAGES, "C");
    gtk_disable_setlocale();
    if (!gtk_init_check(NULL, NULL)) fatal("Cannot open a graphical display. Use --print-pipeline to check without a GUI.");
    apply_receiver_theme();
    a->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(a->window), "Linrad DVB · PE1ITR");
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) monitor = gdk_display_get_monitor(display, 0);
    GdkRectangle work = {0, 0, 1600, 1000};
    if (monitor) gdk_monitor_get_workarea(monitor, &work);
    gtk_window_set_default_size(GTK_WINDOW(a->window), work.width*0.83, work.height*0.8);
    gtk_window_set_position(GTK_WINDOW(a->window), GTK_WIN_POS_CENTER);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(a->window), root);
    GError *usb_error=NULL;
    a->usb_receiver=usb_receiver_new(&usb_error);
    /* These are direct dialog actions: buttons open on the first click without
     * the selection/grab lifecycle of a GtkMenuShell. */
    GtkWidget *menu = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(menu, "receiver-menu");
    GtkWidget *config_item = gtk_button_new_with_label("Input"), *about_item = gtk_button_new_with_label("About");
    gtk_box_pack_start(GTK_BOX(menu), config_item, FALSE, FALSE, 0);
    GtkWidget *mode_item = gtk_button_new_with_label("Mode"), *renderer_item = gtk_button_new_with_label("Renderer");
    gtk_box_pack_start(GTK_BOX(menu), mode_item, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(menu), renderer_item, FALSE, FALSE, 0);
    GtkWidget *tci_item=gtk_button_new_with_label("TCI");
    gtk_box_pack_start(GTK_BOX(menu),tci_item,FALSE,FALSE,0);
    gtk_widget_set_tooltip_text(tci_item,"Open TCI · Alt+T");
    g_signal_connect(tci_item,"clicked",G_CALLBACK(show_tci),a);
    gtk_box_pack_start(GTK_BOX(menu), about_item, FALSE, FALSE, 0);
    gtk_widget_set_tooltip_text(config_item, "Open Input · Alt+I");
    gtk_widget_set_tooltip_text(mode_item, "Open Mode · Alt+M");
    gtk_widget_set_tooltip_text(renderer_item, "Open Renderer · Alt+R");
    gtk_widget_set_tooltip_text(about_item, "Open About · Alt+A");
    g_signal_connect(mode_item, "clicked", G_CALLBACK(show_mode), a);
    g_signal_connect(renderer_item, "clicked", G_CALLBACK(show_renderer), a);
    GtkWidget *top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(root), top_bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top_bar), menu, FALSE, FALSE, 0);
    a->mode_summary = gtk_label_new("");
    gtk_widget_set_name(a->mode_summary, "mode-display");
    gtk_widget_set_margin_start(a->mode_summary, 24);
    gtk_label_set_xalign(GTK_LABEL(a->mode_summary),0);
    gtk_label_set_ellipsize(GTK_LABEL(a->mode_summary),PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(top_bar),a->mode_summary,TRUE,TRUE,6);
    a->frequency_display = gtk_label_new("");
    gtk_widget_set_name(a->frequency_display, "frequency-display");
    gtk_widget_set_tooltip_text(a->frequency_display, "Receive frequency · ← / → to tune; when using Linrad, tune in Linrad.");
    gtk_box_pack_start(GTK_BOX(top_bar), a->frequency_display, FALSE, FALSE, 8);
    a->gain_display = gtk_label_new("");
    gtk_widget_set_name(a->gain_display, "gain-display");
    gtk_widget_set_tooltip_text(a->gain_display, "↑ / ↓ increase / decrease gain · direct SDR input only.");
    gtk_box_pack_start(GTK_BOX(top_bar), a->gain_display, FALSE, FALSE, 8);
    a->power_button = gtk_button_new();
    gtk_widget_set_name(a->power_button, "receiver-power");
    gtk_widget_set_margin_end(a->power_button, 6);
    gtk_widget_set_margin_top(a->power_button, 3);
    gtk_widget_set_margin_bottom(a->power_button, 3);
    GtkWidget *power_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *lamp = gtk_label_new("●");
    gtk_style_context_add_class(gtk_widget_get_style_context(lamp), "power-lamp");
    a->power_label = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(power_content), lamp, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(power_content), a->power_label, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(a->power_button), power_content);
    gtk_box_pack_end(GTK_BOX(top_bar), a->power_button, FALSE, FALSE, 0);
    a->record_button = gtk_toggle_button_new_with_label("● Record IQ");
    gtk_widget_set_name(a->record_button, "iq-record");
    gtk_widget_set_margin_top(a->record_button, 3);
    gtk_widget_set_margin_bottom(a->record_button, 3);
    gtk_box_pack_end(GTK_BOX(top_bar), a->record_button, FALSE, FALSE, 0);
    g_signal_connect(a->record_button, "toggled", G_CALLBACK(record_toggled), a);
    power_refresh(a);
    g_signal_connect(config_item, "clicked", G_CALLBACK(show_config), a);
    g_signal_connect(about_item, "clicked", G_CALLBACK(show_about), a);

    /* Full-width waterfall + spectrum above decoder information and video. */
    a->dashboard = gtk_grid_new();
    gtk_grid_set_row_homogeneous(GTK_GRID(a->dashboard), TRUE);
    gtk_box_pack_start(GTK_BOX(root), a->dashboard, TRUE, TRUE, 0);
    GtkWidget *view = iq_view_create(&a->iq_view, a->config);
    gtk_widget_add_events(a->iq_view.area,GDK_BUTTON_PRESS_MASK);
    g_signal_connect(a->iq_view.area,"button-press-event",G_CALLBACK(usb_spectrum_clicked),a);
    a->filter_box = a->iq_view.filter_toggle;
    a->scatter_box = gtk_toggle_button_new_with_label("TSscatterFix");
    gtk_widget_set_name(a->scatter_box,"spectrum-scatter");
    atk_object_set_name(gtk_widget_get_accessible(a->scatter_box),"TSscatterFix on/off");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->scatter_box),a->scatter);
    gtk_box_pack_start(GTK_BOX(a->iq_view.filter_controls),a->scatter_box,FALSE,FALSE,0);
    ts_info_view_create(&a->information, GTK_WINDOW(a->window), a->iq_view.filter_controls);
    a->usb_toggle=gtk_toggle_button_new_with_label("USB");
    gtk_widget_set_name(a->usb_toggle,"spectrum-usb");
    atk_object_set_name(gtk_widget_get_accessible(a->usb_toggle),"USB receiver on/off");
    gtk_widget_set_tooltip_text(a->usb_toggle,usb_error ? usb_error->message :
        "USB (U) · 3 kHz · Page Up / Page Down: -100 / +100 Hz");
    gtk_widget_set_sensitive(a->usb_toggle,a->usb_receiver!=NULL);
    gtk_box_pack_start(GTK_BOX(a->iq_view.filter_controls),a->usb_toggle,FALSE,FALSE,0);
    g_signal_connect(a->usb_toggle,"toggled",G_CALLBACK(usb_toggled),a);
    a->fm_toggle=gtk_toggle_button_new_with_label("FM");
    gtk_widget_set_name(a->fm_toggle,"spectrum-fm");
    atk_object_set_name(gtk_widget_get_accessible(a->fm_toggle),"FM receiver on/off");
    gtk_widget_set_tooltip_text(a->fm_toggle,usb_error ? usb_error->message :
        "FM (F) · 12.5 kHz · Page Up / Page Down: -2.5 / +2.5 kHz");
    gtk_widget_set_sensitive(a->fm_toggle,a->usb_receiver!=NULL);
    gtk_box_pack_start(GTK_BOX(a->iq_view.filter_controls),a->fm_toggle,FALSE,FALSE,0);
    g_signal_connect(a->fm_toggle,"toggled",G_CALLBACK(usb_toggled),a);
    a->fm_data_toggle=gtk_toggle_button_new_with_label("FM-DATA");
    gtk_widget_set_name(a->fm_data_toggle,"spectrum-fm-data");
    atk_object_set_name(gtk_widget_get_accessible(a->fm_data_toggle),"FM-DATA receiver on/off");
    gtk_widget_set_tooltip_text(a->fm_data_toggle,usb_error ? usb_error->message :
        "FM-DATA (D) · 20 kHz · flat audio · Page Up / Page Down: -2.5 / +2.5 kHz");
    gtk_widget_set_sensitive(a->fm_data_toggle,a->usb_receiver!=NULL);
    gtk_box_pack_start(GTK_BOX(a->iq_view.filter_controls),a->fm_data_toggle,FALSE,FALSE,0);
    g_signal_connect(a->fm_data_toggle,"toggled",G_CALLBACK(usb_toggled),a);
    gtk_widget_set_hexpand(view, TRUE); gtk_widget_set_vexpand(view, TRUE);
    gtk_grid_attach(GTK_GRID(a->dashboard), view, 0, 0, 1, 1);
    a->lower = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(a->lower), 6);
    gtk_widget_set_hexpand(a->lower, TRUE); gtk_widget_set_vexpand(a->lower, TRUE);
    GtkWidget *lower_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lower_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(lower_scroll), a->lower);
    gtk_grid_attach(GTK_GRID(a->dashboard), lower_scroll, 0, 1, 1, 1);
    /* Match Service info's natural width and height without consuming spare
     * horizontal space; future panels can join this row. */
    GtkWidget *receiver_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(receiver_row, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(receiver_row), decoder_view_create(&a->decoder_view), FALSE, FALSE, 0);
    GtkWidget *video_frame = video_view_create(&a->video_view);
    char *gpu_program = setting(a, "ffplay", "gpu_program"), *gpu_device = setting(a, "ffplay", "gpu_device");
    video_view_set_gpu_player(&a->video_view, gpu_program, gpu_device);
    g_free(gpu_program); g_free(gpu_device);
    gtk_widget_set_hexpand(video_frame, FALSE);
    GtkSizeGroup *panel_width = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    gtk_size_group_add_widget(panel_width, a->decoder_view.service_frame);
    gtk_size_group_add_widget(panel_width, video_frame);
    g_object_unref(panel_width);
    gtk_box_pack_start(GTK_BOX(receiver_row), video_frame, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(receiver_row), scatter_view_create(&a->scatter_view), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(receiver_row), program_view_create(&a->programs, select_program, a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(a->lower), receiver_row, FALSE, FALSE, 0);
    create_mode_renderer(a);
    create_tci(a,usb_error ? usb_error->message : NULL);
    g_clear_error(&usb_error);
    populate_rates(a); populate_fecs(a);
    a->status = gtk_label_new("");
    gtk_widget_set_no_show_all(a->status, TRUE);
    gtk_label_set_xalign(GTK_LABEL(a->status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(a->status), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(a->lower), a->status, FALSE, FALSE, 0);
    GtkWidget *diagnostics = gtk_expander_new("Startup command / Log");
    GtkWidget *diagnostic_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(diagnostics), diagnostic_box);
    gtk_box_pack_start(GTK_BOX(a->lower), diagnostics, FALSE, FALSE, 0);
    a->preview = text_panel(diagnostic_box, "Startup command", 55);
    a->log = text_panel(diagnostic_box, "Log", 80);
    create_config(a);
    source_changed(a->source_box, a);
    g_signal_connect(a->receiver_offset_box,"value-changed",G_CALLBACK(tuning_changed),a);
    g_signal_connect(a->offset_box, "value-changed", G_CALLBACK(tuning_changed), a);
    g_signal_connect(a->step_box, "value-changed", G_CALLBACK(tuning_changed), a);
    g_signal_connect(a->window, "key-press-event", G_CALLBACK(tuning_key), a);
    g_signal_connect(a->filter_box, "toggled", G_CALLBACK(filter_changed), a);
    g_signal_connect(a->format_box, "changed", G_CALLBACK(source_changed), a);
    g_signal_connect(a->source_box, "changed", G_CALLBACK(source_changed), a);
    g_signal_connect(a->sdriq_rate_box, "changed", G_CALLBACK(source_changed), a);
    g_signal_connect(a->rate_box, "value-changed", G_CALLBACK(source_changed), a);
    g_signal_connect(a->frequency_box, "value-changed", G_CALLBACK(source_changed), a);
    g_signal_connect(a->constellation_box, "changed", G_CALLBACK(constellation_changed), a);
    g_signal_connect(a->standard_box, "changed", G_CALLBACK(standard_changed), a);
    g_signal_connect(a->sr_box, "changed", G_CALLBACK(selection_changed), a);
    g_signal_connect(a->fec_box, "changed", G_CALLBACK(selection_changed), a);
    g_signal_connect(a->gi_box, "changed", G_CALLBACK(selection_changed), a);
    g_signal_connect(a->scatter_box, "toggled", G_CALLBACK(scatter_changed), a);
    g_signal_connect(a->renderer_box, "changed", G_CALLBACK(renderer_selection_changed), a);
    g_signal_connect(a->power_button, "clicked", G_CALLBACK(power_clicked), a);
    g_signal_connect(a->window, "delete-event", G_CALLBACK(window_close), a);
    a->iq_view.timer = g_timeout_add(100, monitor_tick, a);
    guint decoder_timer = g_timeout_add(50, decoder_tick, a);
    monitor_tick(a); decoder_tick(a); refresh_preview(a); gtk_widget_show_all(a->window); gtk_main();
    g_source_remove(decoder_timer);
    g_source_remove(a->iq_view.timer);
    usb_receiver_free(a->usb_receiver); a->usb_receiver=NULL;
    decoder_view_stop(&a->decoder_view);
    video_view_destroy(&a->video_view);
    scatter_view_reset(&a->scatter_view);
    if (a->iq_view.fd >= 0) close(a->iq_view.fd);
    ts_info_view_destroy(&a->information);
    gtk_widget_destroy(a->mode_dialog);
    gtk_widget_destroy(a->renderer_dialog);
    gtk_widget_destroy(a->tci_dialog);
    gtk_widget_destroy(a->config_dialog);
    gtk_widget_destroy(a->window);
}

static int choice(const char *value, const char **choices, int n, const char *name) {
    for (int i = 0; i < n; ++i) if (!strcmp(value, choices[i])) return i;
    fatal("Invalid %s: %s", name, value);
}

static void load_session(App *a) {
    if (g_key_file_has_key(a->config, "session", "renderer", NULL)) {
        char *renderer = setting(a, "session", "renderer");
        a->renderer = choice(renderer, renderers, G_N_ELEMENTS(renderers), "renderer");
        g_free(renderer);
    }
    const char *keys[] = {"source", "standard", "constellation", "fec", "gi"};
    const char **lists[] = {sources, standards, constellations, fecs, guards};
    int counts[] = {G_N_ELEMENTS(sources), G_N_ELEMENTS(standards), G_N_ELEMENTS(constellations),
                    G_N_ELEMENTS(fecs), G_N_ELEMENTS(guards)};
    int *values[] = {&a->source, &a->standard, &a->constellation, &a->fec, &a->gi};
    for (unsigned i = 0; i < G_N_ELEMENTS(keys); ++i) {
        if (!g_key_file_has_key(a->config, "session", keys[i], NULL)) continue;
        char *value = setting(a, "session", keys[i]);
        *values[i] = choice(value, lists[i], counts[i], keys[i]); g_free(value);
    }
    a->sr = g_key_file_has_key(a->config, "session", "sr", NULL) ?
        integer_setting(a, "session", "sr", 1, 100000000) : (a->standard == 2 ? 150000 : 125000);
    if (g_key_file_has_key(a->config, "session", "scatterfix", NULL)) {
        GError *error = NULL;
        a->scatter = g_key_file_get_boolean(a->config, "session", "scatterfix", &error);
        if (error) fatal("Config [session] scatterfix: %s", error->message);
    }
}

int main(int argc, char **argv) {
    if (argc>1 && !strcmp(argv[1],"--decoder-worker=leansdr"))
        return leansdr_backend_main(argc-1,argv+1);
    if (argc > 1 && !strcmp(argv[1], "--decoder-worker=dvbt"))
        return dvbt_backend_main(argc-1, argv+1);
    App a = {.standard = 0, .sr = 125000, .tune_fd = -1};
    char *source = NULL, *constellation = NULL, *renderer = NULL;
    char *config = NULL, *standard = NULL, *fec = NULL, *gi = NULL, *iq_file = NULL;
    gboolean print = FALSE, run = FALSE, scatter = FALSE, no_scatter = FALSE;
    int sr = -1;
    GOptionEntry entries[] = {
        {"renderer",0,0,G_OPTION_ARG_STRING,&renderer,"embedded, udp, cvlc, vlc, ffplay, custom or mpegts","OUTPUT"},
        {"iq-file",0,0,G_OPTION_ARG_FILENAME,&iq_file,"Play an IQ file in real time (with JSON metadata)","FILE"},
        {"constellation", 0, 0, G_OPTION_ARG_STRING, &constellation, "DVB-S2: QPSK or 8PSK", "MODULATION"},
        {"source", 0, 0, G_OPTION_ARG_STRING, &source, "linrad, hackrf, b210, sdriq, afedri, rtlsdr, fcdpp, lime, rsp1a, rsp1b, airspyhf, airspyr2 or file", "SOURCE"},
        {"config", 'c', 0, G_OPTION_ARG_FILENAME, &config, "Custom configuration file", "FILE"},
        {"standard", 0, 0, G_OPTION_ARG_STRING, &standard, "DVB-S, DVB-S2 or DVB-T", "MODE"},
        {"sr", 0, 0, G_OPTION_ARG_INT, &sr, "Symbol rate (35000, 66000, 125000, 250000, 333000, 500000, 1000000, 2000000, 4000000, 5000000; DVB-T BW: 35000, 40000, 150000, 250000, 333000, 500000)", "RATE"},
        {"fec", 0, 0, G_OPTION_ARG_STRING, &fec, "FEC supported by the standard/constellation (e.g. 3/4)", "FEC"},
        {"gi", 0, 0, G_OPTION_ARG_STRING, &gi, "DVB-T: 1/32, 1/16, 1/8 or auto", "GI"},
        {"no-scatterfix", 0, 0, G_OPTION_ARG_NONE, &no_scatter, "Disable the saved ScatterFix setting", NULL},
        {"scatterfix", 0, 0, G_OPTION_ARG_NONE, &scatter, "Enable ScatterFix", NULL},
        {"print-pipeline", 0, 0, G_OPTION_ARG_NONE, &print, "Show the pipeline without starting processes", NULL},
        {"run", 0, 0, G_OPTION_ARG_NONE, &run, "Run without a GUI; stop with Ctrl+C", NULL}, {NULL}
    };
    GOptionContext *context = g_option_context_new("- Linrad DVB receiver with embedded DVB-S/S2/T decoders");
    g_option_context_set_description(context,
        "DVB-S2 LDPC method, worker count and latency are configured in Mode or [leandvb] in the INI file.\n"
        "Direct decoder help (IQ on stdin, TS on stdout):\n"
        "  linrad_dvb --decoder-worker=leansdr --help\n"
        "  linrad_dvb --decoder-worker=dvbt --help\n"
        "Bundled LDPC helper: ldpc_tool --help / --version");
    g_option_context_add_main_entries(context, entries, NULL);
    GError *error = NULL;
    if (!g_option_context_parse(context, &argc, &argv, &error)) fatal("%s", error->message);
    if (argc != 1 || (print && run)) fatal("Invalid arguments; see --help");
    if (!config) {
        char *exe = g_file_read_link("/proc/self/exe", &error);
        if (!exe) fatal("Cannot determine the program directory: %s", error->message);
        char *dir = g_path_get_dirname(exe); config = g_build_filename(dir, "..", "linrad-dvb.ini", NULL);
        g_free(exe); g_free(dir);
    }
    a.config_path = g_canonicalize_filename(config, NULL); a.directory = g_path_get_dirname(a.config_path);
    a.config = g_key_file_new();
    if (!g_key_file_load_from_file(a.config, a.config_path, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, &error)) fatal("%s: %s", a.config_path, error->message);
    source_defaults(&a);
    load_session(&a);
    if (renderer) {
        a.renderer = choice(renderer, renderers, G_N_ELEMENTS(renderers), "renderer");
        g_key_file_set_string(a.config, "session", "renderer", renderer);
        g_free(renderer);
    }
    if (source) a.source = choice(source, sources, G_N_ELEMENTS(sources), "input");
    if (iq_file && !select_iq_file(&a,iq_file,&error)) fatal("%s",error->message);
    if (standard) {
        int selected = choice(standard, standards, G_N_ELEMENTS(standards), "standard");
        if (selected != a.standard) {
            a.standard = selected; a.constellation = 0; a.fec = 0;
            a.sr = a.standard == 2 ? 150000 : 125000;
        }
    }
    if (constellation) {
        a.constellation = choice(constellation, constellations, 2, "constellation");
        if (!fec && !valid_fec(&a, a.fec)) a.fec = a.constellation ? 1 : 0;
    }
    g_free(constellation);
    if (a.constellation && a.standard != 1) fatal("8PSK requires DVB-S2");
    if (fec) a.fec = choice(fec, fecs, G_N_ELEMENTS(fecs), "FEC");
    if (!valid_fec(&a, a.fec)) fatal("Invalid FEC for the selected standard/constellation");
    if (gi) a.gi = choice(gi, guards, 4, "guard interval");
    if (sr != -1) a.sr = sr;
    const int *rates = a.standard == 2 ? terrestrial_rates : sat_rates;
    gboolean valid = FALSE;
    for (int i = 0; i < (a.standard == 2 ? (int)G_N_ELEMENTS(terrestrial_rates) : (int)G_N_ELEMENTS(sat_rates)); ++i)
        if (a.sr == rates[i]) valid = TRUE;
    if (!valid && !(a.source==SOURCE_FILE && a.sr>0 && a.sr<=100000000)) fatal("Invalid SR %d for %s", a.sr, standards[a.standard]);
    if (scatter && no_scatter) fatal("Choose either --scatterfix or --no-scatterfix");
    if (scatter) a.scatter = TRUE;
    if (no_scatter) a.scatter = FALSE;
    a.headless = run;
    g_free(source); g_free(iq_file);
    const char *offset_problem=receiver_offset_problem(&a);
    if ((print || run) && offset_problem) fatal("%s",offset_problem);
    if (!offset_problem) build_pipeline(&a);
    if (print) { char *text = describe(&a); puts(text); g_free(text); }
    else {
        signal(SIGPIPE, SIG_IGN);
        guint sigint = g_unix_signal_add(SIGINT, exit_signal, &a), sigterm = g_unix_signal_add(SIGTERM, exit_signal, &a);
        if (run) {
            a.loop = g_main_loop_new(NULL, FALSE); start_pipeline(&a);
            guint output_timer = g_timeout_add(100, decoder_tick, &a);
            if (a.running) g_main_loop_run(a.loop);
            g_source_remove(output_timer);
            g_main_loop_unref(a.loop);
        } else gui(&a);
        g_source_remove(sigint); g_source_remove(sigterm);
    }
    clear_stages(&a); g_key_file_unref(a.config); g_free(a.config_path); g_free(a.directory);
    g_free(config); g_free(standard); g_free(fec); g_free(gi); g_option_context_free(context);
    return a.exit_code;
}
