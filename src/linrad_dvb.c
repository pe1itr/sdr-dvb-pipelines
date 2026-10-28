#define _POSIX_C_SOURCE 200809L
#include <gtk/gtk.h>
#include "gui_theme.h"
#include "sdriq.h"
#include "iq_file.h"
#include <glib-unix.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "iq_view.h"
#include "decoder_view.h"
#include "dvbt_backend.h"

typedef struct { GPtrArray *args; GPid pid, group; } Stage;
typedef struct {
    GKeyFile *config;
    char *config_path, *directory;
    int standard, sr, fec, gi, exit_code, source, constellation;
    GtkWidget *source_box, *rate_box, *sdriq_rate_box, *frequency_box, *format_box;
    GtkWidget *frequency_display, *frequency_calculation, *gain_display, *offset_box, *step_box;
    GtkWidget *filter_box;
    GtkWidget *config_dialog, *hardware, *hardware_stack, *renderer_panel, *renderer_box;
    GtkWidget *hardware_entries[4][3], *ip_box, *port_box, *header_box, *center_box;
    GtkWidget *sr_label, *dashboard, *lower;
    GtkWidget *mode_dialog, *renderer_dialog, *mode_summary;
    GtkWidget *renderer_program, *renderer_args, *output_file, *renderer_error;
    GtkWidget *file_path, *file_format, *file_header, *file_center, *file_details;
    int renderer;
    int tune_fd;
    gboolean scatter, running, stopping, closing, headless;
    guint kill_timer, drain_timer;
    gboolean replay_eof;
    Stage stages[4];
    int stage_count;
    GtkWidget *window, *controls, *standard_box, *sr_box, *fec_box, *gi_box, *constellation_box;
    GtkWidget *scatter_box, *power_button, *power_label, *status, *preview, *log;
    GMainLoop *loop;
    IQView iq_view;
    DecoderView decoder_view;
} App;

static const char *standards[] = {"DVB-S", "DVB-S2", "DVB-T"};
static const char *constellations[] = {"QPSK", "8PSK"};
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
enum { SOURCE_FILE = 4 };
static const char *sources[] = {"linrad", "hackrf", "b210", "sdriq", "file"};
static const char *source_labels[] = {"linrad", "hackrf", "b210", "sdriq", "Bestand (IQ)"};
static const char *file_formats[] = {"s16", "u8", "cf32"};
static gboolean hardware_source(App *a) { return a->source>0 && a->source<SOURCE_FILE; }
enum { RENDER_CVLC, RENDER_VLC, RENDER_FFPLAY, RENDER_CUSTOM, RENDER_FILE };
static const char *renderers[] = {"cvlc", "vlc", "ffplay", "custom", "mpegts"};
static const char *ffplay_args = "-hide_banner -loglevel warning -autoexit -f mpegts";
static gboolean use_scatter(App *a) { return a->scatter && a->renderer != RENDER_FILE; }
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
    if (!valid_fec(a, a->fec)) fatal("Ongeldige FEC voor de gekozen standaard/constellatie");
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
        sample_rate = integer_setting(a, section, "sample_rate", a->source == 3 ? 8138 : 2000000,
                                      a->source == 3 ? 196078 : a->source == 1 ? 20000000 : 61440000);
        if (a->source == 3 && !sdriq_rate_valid(sample_rate)) fatal("Ongeldige SDR-IQ samplerate: %d", sample_rate);
        g_free(format); format = setting(a, section, "format");
        if (strcmp(format, "s16") && strcmp(format, "u8")) fatal("Invalid SDR format");
        s = new_stage(a, "sdr");
        add(s, "--source"); add(s, section);
        if (a->window) add(s, "--control-stdin");
        add(s, "--format"); add(s, format);
        add_number(s, "--sample-rate", sample_rate);
        const char *keys[] = {"frequency", "gain", "args", "images", "antenna"};
        const char *flags[] = {"--frequency", "--gain", "--args", "--images", "--antenna"};
        for (int i = 0; i < 5; ++i) {
            char *value = setting(a, section, keys[i]);
            add(s, flags[i]); add(s, value); g_free(value);
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
        if (!self) fatal("Kan de ingebouwde DVB-T-decoder niet vinden");
        add(s, self); g_free(self); add(s, "--decoder-worker=dvbt");
        options(a, s, "rbdvbt_rx", "args", "--stdin --input-format --sample-rate --sr --gi --fec --stdout-ts --ts-out --udp-ts --udp-out --visualizer-udp");
        /* These are essential for the integrated live decode path, even when
         * an older/empty config does not supply them. */
        add(s, "--live"); add(s, "--resample-to-dvbt-rate");
        add(s, "--stdin"); add(s, "--input-format"); add(s, format);
        add_number(s, "--sample-rate", sample_rate); add_number(s, "--sr", a->sr);
        add(s, "--gi"); add(s, guards[a->gi]); add(s, "--fec"); add(s, fecs[a->fec]); add(s, "--stdout-ts");
    } else {
        s = new_stage(a, "leandvb");
        options(a, s, "leandvb", a->standard ? "dvbs2_args" : "dvbs_args",
                "--s16 --u8 --f32 -f --standard --sr --cr --modcods");
        add(s, !strcmp(format, "u8") ? "--u8" : "--s16"); add_number(s, "-f", sample_rate);
        add(s, "--standard"); add(s, standards[a->standard]); add_number(s, "--sr", a->sr);
        if (a->standard == 0) { add(s, "--cr"); add(s, fecs[a->fec]); }
        else {
            char *mask = g_strdup_printf("0x%x", 1u << modcods[a->constellation][a->fec]);
            add(s, "--modcods"); add(s, mask); g_free(mask);
        }
    }
    char *vlc_input = g_strdup("fd://0");
    if (use_scatter(a)) {
        s = new_stage(a, "scatterfix");
        options(a, s, "scatterfix", "args", "--udp-output");
        char *host = setting(a, "scatterfix", "udp_host");
        if (inet_pton(AF_INET, host, &addr) != 1) fatal("Config [scatterfix] udp_host must be a local IPv4 address");
        int port = integer_setting(a, "scatterfix", "udp_port", 1, 65535);
        char *destination = g_strdup_printf("%s:%d", host, port);
        add(s, "--udp-output"); add(s, destination);
        g_free(vlc_input); vlc_input = g_strdup_printf("udp://@%s:%d", host, port);
        g_free(host); g_free(destination);
    }
    if (a->renderer == RENDER_CUSTOM) {
        s = new_stage(a, "custom_renderer");
        options(a, s, "renderer", "args", "");
        /* The renderer chooses its own stdin syntax via {input}; no shell. */
        char *input = use_scatter(a) ? g_strdup(vlc_input) : g_strdup("-");
        if (use_scatter(a)) memmove(input+6, input+7, strlen(input+7)+1); /* udp://host:port */
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
        /* FFmpeg uses pipe:0 for stdin and UDP URLs without VLC's @. */
        char *input = use_scatter(a) ? g_strdup(vlc_input) : g_strdup("pipe:0");
        if (use_scatter(a)) memmove(input+6, input+7, strlen(input+7)+1);
        add(s, input); g_free(input);
    } else if (a->renderer != RENDER_FILE) {
        s = new_stage(a, renderers[a->renderer]); options(a, s, "cvlc", "args", ""); add(s, vlc_input);
    }
    for (int i = 0; i < a->stage_count; ++i) g_ptr_array_add(a->stages[i].args, NULL);
    g_free(vlc_input); g_free(format); g_free(ip);
}

static char *describe(App *a) {
    GString *text = g_string_new(NULL);
    for (int i = 0; i < a->stage_count; ++i) {
        if (i) g_string_append(text, use_scatter(a) && i == a->stage_count - 1 ? "\n  [UDP MPEG-TS] ->\n" : "\n  |\n");
        char **args = (char **)a->stages[i].args->pdata;
        for (int j = 0; args[j]; ++j) {
            char *quoted = g_shell_quote(args[j]);
            if (j) g_string_append_c(text, ' ');
            g_string_append(text, quoted); g_free(quoted);
        }
    }
    if (a->renderer == RENDER_FILE) {
        char *path = setting(a, "renderer", "output_file");
        char *resolved = g_canonicalize_filename(path, a->directory), *quoted = g_shell_quote(resolved);
        g_string_append_printf(text, "\n  > %s  (nieuw MPEG-TS-bestand)", quoted);
        g_free(path); g_free(resolved); g_free(quoted);
    }
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

static gboolean read_log(gint fd, GIOCondition condition, gpointer data) {
    App *a = data;
    char buf[4097]; ssize_t n;
    /* Bound each callback so continuous decoder logging cannot starve Stop. */
    for (int i = 0; i < 16; ++i) {
        n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) { buf[n] = 0; log_text(a, buf); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (!n || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { close(fd); return G_SOURCE_REMOVE; }
        break;
    }
    if (condition & G_IO_NVAL) { close(fd); return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

static void scatter_refresh(App *a) {
    if (!a->scatter_box) return;
    gtk_widget_set_sensitive(a->scatter_box, !a->running && !a->stopping && a->renderer != RENDER_FILE);
    gtk_widget_set_tooltip_text(a->scatter_box, a->renderer == RENDER_FILE ?
        "Scatterfix is niet beschikbaar bij rechtstreekse MPEG-TS-bestandsuitvoer." :
        a->running || a->stopping ? "Stop de ontvangst om Scatterfix te wijzigen." :
        a->scatter ? "Scatterfix aan · klik om uit te schakelen." : "Scatterfix uit · klik om in te schakelen.");
}

static void power_refresh(App *a) {
    scatter_refresh(a);
    if (!a->power_button) return;
    GtkStyleContext *style = gtk_widget_get_style_context(a->power_button);
    const char *states[] = {"receiver-off", "receiver-running", "receiver-stopping", "receiver-error"};
    for (unsigned i=0; i<G_N_ELEMENTS(states); ++i) gtk_style_context_remove_class(style, states[i]);
    int state = a->stopping ? 2 : a->running ? 1 : a->exit_code ? 3 : 0;
    gtk_style_context_add_class(style, states[state]);
    const char *labels[] = {"Start SDR", "Stop SDR · actief", "SDR stopt…", "Start SDR · fout"};
    const char *file_labels[] = {"Start bestand", "Stop bestand · actief", "Bestand stopt…", "Start bestand · fout"};
    const char *tips[] = {"Ontvanger uit. Klik om te starten.",
        "Ontvangstpipeline draait. Klik om te stoppen.",
        "De ontvangstpipeline wordt gestopt. Even wachten…",
        "Ontvangst gestopt door een fout. Zie de status en het log; klik om opnieuw te starten."};
    gtk_label_set_text(GTK_LABEL(a->power_label), a->source==SOURCE_FILE ? file_labels[state] : labels[state]);
    gtk_widget_set_tooltip_text(a->power_button, tips[state]);
    gtk_widget_set_sensitive(a->power_button, !a->stopping);
}

static void finish_if_stopped(App *a) {
    if (!a->stopping || a->kill_timer) return;
    for (int i = 0; i < a->stage_count; ++i) if (a->stages[i].pid) return;
    if (a->tune_fd >= 0) { close(a->tune_fd); a->tune_fd = -1; }
    a->running = a->stopping = FALSE;
    if (!a->exit_code) status(a, a->replay_eof ? "IQ-bestand volledig afgespeeld" : "Gestopt");
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
    a->kill_timer = 0; finish_if_stopped(a); return G_SOURCE_REMOVE;
}

static void stop_pipeline(App *a) {
    if (!a->running || a->stopping) return;
    a->stopping = TRUE;
    decoder_view_stop(&a->decoder_view);
    decoder_view_tick(&a->decoder_view, FALSE, standards[a->standard]);
    if (a->drain_timer) { g_source_remove(a->drain_timer); a->drain_timer=0; }
    if (a->tune_fd >= 0) { close(a->tune_fd); a->tune_fd = -1; }
    if (!a->exit_code) status(a, "Pipeline stoppen...");
    power_refresh(a);
    for (int i = 0; i < a->stage_count; ++i)
        if (a->stages[i].group > 0) kill(-a->stages[i].group, SIGTERM);
    a->kill_timer = g_timeout_add(1500, force_stop, a);
}

static gboolean replay_drained(gpointer data) {
    App *a=data; a->drain_timer=0;
    if (a->stages[1].pid) {
        a->exit_code=1; status(a,"Decoder stopt niet na het einde van het IQ-bestand");
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
            if (i<2) a->replay_eof=TRUE;
            if (a->tune_fd>=0 && !a->stages[0].pid) { close(a->tune_fd); a->tune_fd=-1; }
            if (a->replay_eof) {
                if (a->drain_timer) g_source_remove(a->drain_timer);
                a->drain_timer=g_timeout_add(a->stages[1].pid ? 10000 : 1500,replay_drained,a);
                status(a,"IQ-bestand uitgelezen; decoder en speler afronden…");
                gboolean alive=FALSE;
                for (int j=0; j<a->stage_count; ++j) if (a->stages[j].pid) alive=TRUE;
                if (!alive) stop_pipeline(a);
            } else stop_pipeline(a);
            break;
        }
        if (!a->stopping) {
            char *message = g_strdup_printf("%s is gestopt (%s %d); volledige pipeline wordt gestopt.",
                (char *)a->stages[i].args->pdata[0], WIFEXITED(result) ? "exitcode" : "signaal",
                WIFEXITED(result) ? WEXITSTATUS(result) : WTERMSIG(result));
            a->exit_code = 1; status(a, message); g_free(message); stop_pipeline(a);
        }
        break;
    }
    g_spawn_close_pid(pid); finish_if_stopped(a);
}

static void child_setup(gpointer unused) {
    (void)unused;
    /* Each process group includes decoder/scatterfix descendants. */
    if (setsid() < 0) _exit(126);
    signal(SIGPIPE, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
}

static void start_pipeline(App *a) {
    if (a->running) return;
    build_pipeline(a); a->exit_code = 0;
    a->replay_eof=FALSE;
    if (a->source==SOURCE_FILE) {
        char *path=setting(a,"file","path"), *fmt=setting(a,"file","input_format");
        char *resolved=g_canonicalize_filename(path,a->directory);
        GError *error=NULL; guint64 samples;
        gboolean valid=*path && iq_file_validate(resolved,fmt,integer_setting(a,"file","header_bytes",0,100000000),&samples,&error);
        g_free(path); g_free(fmt); g_free(resolved);
        if (!valid) { a->exit_code=1; status(a,error ? error->message : "Kies eerst een IQ-bestand onder SDR"); g_clear_error(&error); return; }
    }
    a->iq_view.rows = 0; a->iq_view.last_frame = 0;
    for (int i = 0; i < a->stage_count; ++i) {
        char *program = a->stages[i].args->pdata[0], *found = g_find_program_in_path(program);
        if (!found) {
            char *message = g_strdup_printf("Programma niet gevonden of niet uitvoerbaar: %s", program);
            status(a, message); g_free(message); a->exit_code = 1; return;
        }
        g_free(found);
    }
    int pipes[2][2], log_pipe[2] = {-1, -1}, tune_pipe[2] = {-1, -1}, made = 0, nullfd = -1, output_fd = -1;
    if (a->renderer == RENDER_FILE) {
        char *path = setting(a, "renderer", "output_file");
        char *resolved = g_canonicalize_filename(path, a->directory);
        output_fd = open(resolved, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (output_fd < 0) {
            char *message = g_strdup_printf("Kan MPEG-TS-bestand niet maken: %s: %s. Kies een nieuwe bestandsnaam.", resolved, g_strerror(errno));
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
    nullfd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (nullfd < 0) goto pipe_error;
    if (!a->headless) {
        if (!g_unix_open_pipe(log_pipe, FD_CLOEXEC, NULL)) goto pipe_error;
        if (!g_unix_set_fd_nonblocking(log_pipe[0], TRUE, NULL)) goto pipe_error;
    }
    if (a->window && a->standard == 2 && !decoder_view_start(&a->decoder_view))
        log_text(a, "Decodertelemetrie niet beschikbaar; ontvangst gaat door.\n");
    a->running = TRUE;
    if (a->controls) {
        gtk_widget_set_sensitive(a->controls, FALSE);
        gtk_widget_set_sensitive(a->hardware, FALSE);
        gtk_widget_set_sensitive(a->renderer_panel, FALSE);
        gtk_widget_set_sensitive(a->frequency_box, FALSE);
    }
    /* Launch consumers first. GLib closes unused pipe descriptors in children. */
    for (int i = a->stage_count - 1; i >= 0; --i) {
        int in = i == 0 || (use_scatter(a) && i == 3) ? nullfd : pipes[i - 1][0];
        if (i == 0 && tune_pipe[0] >= 0) in = tune_pipe[0];
        int out = i == 1 && output_fd >= 0 ? output_fd : i < 2 ? pipes[i][1] : nullfd;
        GError *error = NULL; Stage *s = &a->stages[i];
        char **env = g_get_environ();
        env = g_environ_unsetenv(env, "LINRAD_IQ_MONITOR_PORT");
        env = g_environ_unsetenv(env, "LINRAD_DECODER_MONITOR_PORT");
        if (i == 1 && a->standard == 2 && a->decoder_view.port) {
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
    if (output_fd >= 0) close(output_fd);
    if (tune_pipe[0] >= 0) close(tune_pipe[0]);
    if (a->stopping) { if (tune_pipe[1] >= 0) close(tune_pipe[1]); }
    else a->tune_fd = tune_pipe[1];
    if (log_pipe[1] >= 0) close(log_pipe[1]);
    if (log_pipe[0] >= 0) g_unix_fd_add(log_pipe[0], G_IO_IN | G_IO_HUP | G_IO_ERR, read_log, a);
    if (!a->stopping) status(a, a->source==SOURCE_FILE ? "IQ-bestand wordt realtime afgespeeld" : "Pipeline actief - wachten op/ontvangen van IQ");
    return;
pipe_error:
    if (output_fd >= 0) close(output_fd);
    if (tune_pipe[0] >= 0) close(tune_pipe[0]);
    if (tune_pipe[1] >= 0) close(tune_pipe[1]);
    for (int i = 0; i < made; ++i) { close(pipes[i][0]); close(pipes[i][1]); }
    if (nullfd >= 0) close(nullfd);
    if (log_pipe[0] >= 0) { close(log_pipe[0]); close(log_pipe[1]); }
    a->exit_code = 1; status(a, "Kan pipeline-pipes niet openen");
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
    build_pipeline(a); char *text = describe(a);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->preview)), text, -1); g_free(text);
    refresh_summaries(a);
}

static void tuning_changed(GtkWidget *widget, gpointer data);

static void tuning_refresh(App *a) {
    if (!a->frequency_display) return;
    gtk_widget_set_sensitive(a->gain_display, hardware_source(a));
    if (hardware_source(a)) {
        char *gain=g_strdup_printf("Gain %.0f dB",g_key_file_get_double(a->config,sources[a->source],"gain",NULL));
        gtk_label_set_text(GTK_LABEL(a->gain_display),gain); g_free(gain);
    } else gtk_label_set_text(GTK_LABEL(a->gain_display),"Gain —");
    gtk_widget_set_sensitive(a->offset_box, hardware_source(a));
    gtk_widget_set_sensitive(a->step_box, hardware_source(a));
    if (a->source==SOURCE_FILE) {
        double frequency=g_key_file_get_double(a->config,"file","frequency",NULL);
        char *text=g_strdup_printf("%.6f MHz",frequency/1e6);
        gtk_label_set_text(GTK_LABEL(a->frequency_display),text); g_free(text);
        gtk_label_set_text(GTK_LABEL(a->frequency_calculation),"Bestandsinvoer: de ontvangstfrequentie bepaalt alleen de schaal; hardware-afstemming en gain zijn uitgeschakeld.");
        return;
    }
    if (!a->source) {
        gtk_label_set_text(GTK_LABEL(a->frequency_display), "Afstemmen in Linrad");
        gtk_label_set_text(GTK_LABEL(a->frequency_calculation), "Stel de hardwarefrequentie en eventuele LO-offset in Linrad in.");
        return;
    }
    const char *section = sources[a->source];
    double offset = g_key_file_get_double(a->config, section, "lo_offset", NULL);
    double step = g_key_file_get_double(a->config, section, "tune_step", NULL);
    double frequency = g_key_file_get_double(a->config, section, "frequency", NULL);
    g_signal_handlers_block_by_func(a->offset_box, tuning_changed, a);
    g_signal_handlers_block_by_func(a->step_box, tuning_changed, a);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->offset_box), offset/1000.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->step_box), step);
    g_signal_handlers_unblock_by_func(a->offset_box, tuning_changed, a);
    g_signal_handlers_unblock_by_func(a->step_box, tuning_changed, a);
    char *text = g_strdup_printf("%.6f MHz", (frequency + offset)/1e6);
    gtk_label_set_text(GTK_LABEL(a->frequency_display), text); g_free(text);
    text = g_strdup_printf("SDR/IF + LO-offset = ontvangstfrequentie\n"
        "%.6f %s %.6f = %.6f MHz\n"
        "Verschil (RF − IF) = %.6f MHz (%.3f kHz)",
        frequency/1e6, offset < 0 ? "−" : "+", fabs(offset)/1e6,
        (frequency+offset)/1e6, offset/1e6, offset/1000.0);
    gtk_label_set_text(GTK_LABEL(a->frequency_calculation), text); g_free(text);
}

static void tuning_changed(GtkWidget *widget, gpointer data) {
    App *a = data; (void)widget;
    if (!hardware_source(a)) return;
    const char *section = sources[a->source];
    g_key_file_set_double(a->config, section, "lo_offset",
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->offset_box))*1000.0);
    g_key_file_set_double(a->config, section, "tune_step",
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->step_box)));
    tuning_refresh(a);
}

static void source_changed(GtkWidget *widget, gpointer data);
static void file_controls_refresh(App *a);

static void tune_step(App *a, int direction) {
    if (!hardware_source(a) || a->stopping) return;
    const char *section = sources[a->source];
    double frequency = g_key_file_get_double(a->config, section, "frequency", NULL);
    double step = g_key_file_get_double(a->config, section, "tune_step", NULL);
    double next = frequency + direction*step;
    double low = a->source == 3 ? 500 : a->source == 1 ? 1000000 : 70000000;
    double high = a->source == 3 ? 30000000 : 6000000000.0;
    if (next < low || next > high) { status(a, "Afstemgrens van de SDR bereikt"); return; }
    if (a->running) {
        char command[64];
        int length = g_snprintf(command, sizeof(command), "%.0f\n", next);
        ssize_t sent;
        do { sent = a->tune_fd < 0 ? -1 : write(a->tune_fd, command, (size_t)length); }
        while (sent < 0 && a->tune_fd >= 0 && errno == EINTR);
        if (sent != length) { status(a, "Afstemopdracht niet verstuurd; probeer opnieuw"); return; }
    }
    g_key_file_set_double(a->config, section, "frequency", next);
    g_signal_handlers_block_by_func(a->frequency_box, source_changed, a);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->frequency_box), next/1000.0);
    g_signal_handlers_unblock_by_func(a->frequency_box, source_changed, a);
    tuning_refresh(a);
    /* Never rebuild Stage objects while their child processes are alive. */
    if (!a->running) refresh_preview(a);
}

static void gain_step(App *a, int direction) {
    if (!hardware_source(a) || a->stopping) return;
    const char *section = sources[a->source];
    double gain = g_key_file_get_double(a->config, section, "gain", NULL);
    double next = gain + direction*(a->source == 3 ? 10 : 1);
    double low = a->source == 3 ? -20 : 0;
    double high = a->source == 3 ? 10 : a->source == 1 ? 116 : 76;
    if (next < low || next > high) { status(a, "Gain-grens van de SDR bereikt"); return; }
    if (a->running) {
        char command[64];
        int length = g_snprintf(command, sizeof(command), "gain %.1f\n", next);
        ssize_t sent;
        do { sent = a->tune_fd < 0 ? -1 : write(a->tune_fd, command, (size_t)length); }
        while (sent < 0 && a->tune_fd >= 0 && errno == EINTR);
        if (sent != length) { status(a, "Gain-opdracht niet verstuurd; probeer opnieuw"); return; }
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
    gtk_widget_set_tooltip_text(a->filter_box, a->iq_view.enabled ?
        "IQ-bandfilter aan. Klik om uit te zetten. [ en ] wijzigen de bandbreedte." :
        "IQ-bandfilter uit. Klik om aan te zetten. [ en ] wijzigen de bandbreedte.");
    if (a->iq_view.area) gtk_widget_queue_draw(a->iq_view.area);
}
static void filter_apply(App *a, int width, gboolean enabled) {
    if (a->stopping) { filter_refresh(a); return; }
    const char *section = sources[a->source];
    if (a->running) {
        char command[64]; int length = g_snprintf(command, sizeof(command), "bandwidth %d\n", enabled ? width : 0);
        ssize_t sent;
        do { sent = a->tune_fd < 0 ? -1 : write(a->tune_fd, command, (size_t)length); }
        while (sent < 0 && a->tune_fd >= 0 && errno == EINTR);
        if (sent != length) { status(a, "Filteropdracht niet verstuurd"); filter_refresh(a); return; }
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
        GtkWidget *fields[] = {a->offset_box, a->step_box, a->frequency_box,
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
    char *message = saved ? g_strdup_printf("Instellingen opgeslagen: %s", a->config_path) :
        g_strdup_printf("Opslaan mislukt: %s", error ? error->message : "onbekende fout");
    status(a, message);
    g_free(message); g_free(contents); g_clear_error(&error);
}

static gboolean tuning_key(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    App *a = data; (void)widget;
    if (event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK)) return FALSE;
    if (event->keyval == GDK_KEY_w || event->keyval == GDK_KEY_W) { save_settings(a); return TRUE; }
    if (event->keyval == GDK_KEY_bracketleft) { filter_step(a, -1); return TRUE; }
    if (event->keyval == GDK_KEY_bracketright) { filter_step(a, 1); return TRUE; }
    if (!hardware_source(a)) return FALSE;
    /* Handle before GTK propagates to focused entries: plain arrows tune
     * throughout this window; modified arrows retain normal editing behavior. */
    if (event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up) { gain_step(a, 1); return TRUE; }
    if (event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down) { gain_step(a, -1); return TRUE; }
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
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(a->frequency_box), a->source == 3 ? 0.5 : a->source == 2 ? 70000 : 1000,
                                  a->source == 3 ? 30000 : 6000000.0);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(a->frequency_box), 3);
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(a->rate_box), a->source == 3 ? 8138 : hardware_source(a) ? 2000000 : 1,
                                  a->source == 3 ? 196078 : a->source == 1 ? 20000000 : a->source == 2 ? 61440000 : 100000000);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->rate_box),
            integer_setting(a, section, "sample_rate", 1, 100000000));
        if (a->source == 3) {
            g_signal_handlers_block_by_func(a->sdriq_rate_box, source_changed, a);
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
        g_key_file_set_integer(a->config, section, "sample_rate", a->source == 3 ?
            atoi(gtk_combo_box_get_active_id(GTK_COMBO_BOX(a->sdriq_rate_box))) :
            gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->rate_box)));
        if (hardware_source(a)) {
            g_key_file_set_double(a->config, section, "frequency", gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->frequency_box)) * 1000.0);
        }
    }
    gtk_widget_set_visible(a->rate_box, a->source != 3);
    gtk_widget_set_visible(a->sdriq_rate_box, a->source == 3);
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
    if (a->running || a->stopping || a->renderer == RENDER_FILE) {
        g_signal_handlers_block_by_func(widget, scatter_changed, a);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget),a->scatter);
        g_signal_handlers_unblock_by_func(widget, scatter_changed, a);
        return;
    }
    selection_changed(widget,data);
}

static void populate_rates(App *a) {
    if (a->sr_label) gtk_label_set_text(GTK_LABEL(a->sr_label), a->standard == 2 ? "BW (Hz)" : "Symbolrate (sym/s)");
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

static gboolean decoder_tick(gpointer data) {
    App *a=data;
    decoder_view_tick(&a->decoder_view, a->running && !a->stopping && a->standard == 2, standards[a->standard]);
    return G_SOURCE_CONTINUE;
}

static gboolean monitor_tick(gpointer data) {
    App *a = data;
    const char *section = sources[a->source];
    a->iq_view.active = a->running && !a->stopping;
    a->iq_view.bandwidth = g_key_file_get_double(a->config, section, "filter_bandwidth", NULL);
    a->iq_view.enabled = g_key_file_get_boolean(a->config, section, "filter_enabled", NULL);
    double center = g_key_file_get_double(a->config, section, "frequency", NULL)
        + g_key_file_get_double(a->config, section, "lo_offset", NULL);
    double rate = g_key_file_get_double(a->config, section, "sample_rate", NULL);
    if (center != a->iq_view.center || rate != a->iq_view.rate) {
        a->iq_view.rows = 0; a->iq_view.last_frame = 0;
    }
    a->iq_view.center = center; a->iq_view.rate = rate;
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
    const char *file_keys[]={"path","input_format","format","sample_rate","header_bytes","frequency","metadata"};
    const char *file_values[]={"","s16","s16","250000","0","0","Geen metadata geladen; controleer de instellingen."};
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
    const char *values[] = {"10000000", "437000000", "30", "", "/home/rhardenb/repos/ettus/uhd-images", "TX/RX", "s16"};
    const char *sdriq_values[] = {"111111", "28000000", "10", "driver=rfspace,sdr-iq=/dev/ttyUSB0", "", "", "s16"};
    for (int j = 1; j < SOURCE_FILE; ++j)
        for (int i = 0; i < 7; ++i)
            if (!g_key_file_has_key(a->config, sources[j], keys[i], NULL))
                g_key_file_set_string(a->config, sources[j], keys[i], j == 3 ? sdriq_values[i] : i == 6 && j == 1 ? "u8" : values[i]);
    for (int j = 1; j < SOURCE_FILE; ++j) {
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
        g_set_error_literal(error,G_OPTION_ERROR,G_OPTION_ERROR_BAD_VALUE,"DVB-mode, modulatie, FEC of guard interval uit de metadata wordt niet ondersteund");
        iq_file_info_clear(&info); return FALSE;
    }
    char *fallback=setting(a,"file","input_format");
    const char *format=*info.format ? info.format : fallback;
    int rate=info.sample_rate ? info.sample_rate : integer_setting(a,"file","sample_rate",1,100000000);
    guint64 samples;
    if (rate<1 || rate>100000000 || !iq_file_validate(info.path,format,info.header_bytes,&samples,error)) {
        if (error && !*error) g_set_error_literal(error,G_OPTION_ERROR,G_OPTION_ERROR_BAD_VALUE,"Ongeldige samplerate");
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
        !info.sample_rate || !*info.format ? " · controleer samplerate en formaat" : "") :
        g_strdup("Geen JSON-metadata. Controleer samplerate, formaat en DVB-instellingen vóór Start.");
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
    else text=g_strdup(error ? error->message : "Kies een IQ-bestand. Afspelen begint met Start bestand rechtsboven.");
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
    GtkWidget *chooser=gtk_file_chooser_dialog_new("IQ-bestand openen",GTK_WINDOW(a->config_dialog),GTK_FILE_CHOOSER_ACTION_OPEN,
        "Annuleren",GTK_RESPONSE_CANCEL,"Openen",GTK_RESPONSE_ACCEPT,NULL);
    GtkFileFilter *filter=gtk_file_filter_new(); gtk_file_filter_set_name(filter,"IQ-bestanden");
    const char *patterns[]={"*.iq","*.cf32","*.fc32","*.s16","*.u8"};
    for (unsigned i=0; i<G_N_ELEMENTS(patterns); ++i) gtk_file_filter_add_pattern(filter,patterns[i]);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser),filter);
    filter=gtk_file_filter_new(); gtk_file_filter_set_name(filter,"Alle bestanden"); gtk_file_filter_add_pattern(filter,"*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser),filter);
    char *current=setting(a,"file","path");
    if (*current) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser),current);
    g_free(current);
    if (gtk_dialog_run(GTK_DIALOG(chooser))==GTK_RESPONSE_ACCEPT) {
        char *path=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        GError *error=NULL;
        if (select_iq_file(a,path,&error)) { file_selection_sync(a); status(a,"IQ-bestand geladen. Controleer Mode; druk op Start bestand om realtime af te spelen."); }
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
    for (int source = 1; source < 4; ++source)
        for (int field = 0; field < 3; ++field)
            if (widget == a->hardware_entries[source][field])
                g_key_file_set_string(a->config, sources[source], keys[field],
                                     gtk_entry_get_text(GTK_ENTRY(widget)));
    if (!a->running) refresh_preview(a);
}

static gboolean apply_hardware(App *a) {
    if (a->running) return TRUE;
    const char *ip = gtk_entry_get_text(GTK_ENTRY(a->ip_box));
    struct in_addr address;
    if (inet_pton(AF_INET, ip, &address) != 1) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(a->config_dialog), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "Vul een geldig IPv4-adres voor Linrad in.");
        gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
        gtk_widget_grab_focus(a->ip_box); return FALSE;
    }
    GtkWidget *fields[] = {a->rate_box, a->port_box, a->header_box, a->center_box,
                           a->frequency_box, a->offset_box, a->step_box, a->file_header, a->file_center};
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
    gtk_widget_set_visible(a->rate_box, a->source != 3);
    gtk_widget_set_visible(a->sdriq_rate_box, a->source == 3);
    gtk_window_present(GTK_WINDOW(a->config_dialog));
}

static void show_info(GtkWidget *widget, gpointer data) {
    (void)widget; App *a = data;
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(a->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "Linrad DVB · PE1ITR");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
        "← / →  ontvangstfrequentie omlaag / omhoog\n"
        "↑ / ↓  gain omhoog / omlaag\n"
        "[ / ]  IQ-bandbreedte kleiner / groter (filter aan)\n"
        "W  configuratie opslaan\n\n"
        "Sneltoetsen werken in het hoofdvenster, ook tijdens ontvangst.\n"
        "Knop rechtsboven: Start SDR / Stop SDR.\n"
        "Lampje: grijs = uit, groen = actief, geel = stoppen, rood = fout.\n"
        "SDR: hardware of Bestand (IQ), samplerate en instellingen.\n"
        "Bestand: kies IQ + JSON-metadata, Start bestand speelt realtime af.\n"
        "Mode: DVB-mode, symbolrate / BW, FEC en modulatie.\n"
        "Renderer: speler, eigen programma of MPEG-TS-bestand.\n"
        "Hardware, DVB en renderer wijzigen bij Stop.\n"
        "Bij Linrad regel je frequentie en gain in Linrad zelf.\n"
        "De ontvangstfrequentie is SDR-frequentie + LO-offset.\n"
        "Waterval: nieuwste regel boven; schaal in MHz.\n\nConfig: %s", a->config_path);
    gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
}

static void create_config(App *a) {
    a->config_dialog = gtk_dialog_new_with_buttons("SDR · input en hardware", GTK_WINDOW(a->window),
        GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, "Sluiten", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(a->config_dialog), 620, -1);
    a->hardware = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(a->hardware), 12);
    gtk_grid_set_row_spacing(GTK_GRID(a->hardware), 8);
    gtk_grid_set_column_spacing(GTK_GRID(a->hardware), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(a->config_dialog))), a->hardware, TRUE, TRUE, 0);
    a->source_box = combo(a->hardware, "Input / SDR", 0, source_labels, G_N_ELEMENTS(source_labels), a->source);
    a->rate_box = spin(a->hardware, "Samplerate (samples/s)", 1, 1, 100000000, 100000);
    a->sdriq_rate_box = gtk_combo_box_text_new();
    gtk_grid_attach(GTK_GRID(a->hardware), a->sdriq_rate_box, 1, 1, 1, 1);
    for (unsigned i = 0; i < G_N_ELEMENTS(sdriq_rates); ++i) {
        char *rate_id = g_strdup_printf("%d", sdriq_rates[i]);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(a->sdriq_rate_box), rate_id, rate_id); g_free(rate_id);
    }
    gtk_widget_set_no_show_all(a->rate_box, TRUE);
    gtk_widget_set_no_show_all(a->sdriq_rate_box, TRUE);
    a->format_box = combo(a->hardware, "IQ-formaat", 2, (const char *[]) {"s16", "u8"}, 2, 0);
    a->hardware_stack = gtk_stack_new();
    gtk_grid_attach(GTK_GRID(a->hardware), a->hardware_stack, 0, 3, 2, 1);
    for (int source=0; source<(int)G_N_ELEMENTS(sources); ++source) {
        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 8); gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
        gtk_stack_add_named(GTK_STACK(a->hardware_stack), grid, sources[source]);
        if (!source) {
            a->ip_box = entry_field(grid, "UDP-bindadres", 0, a, "linrad", "ip");
            a->port_box = spin(grid, "UDP-poort", 1, 1, 65535, 1);
            a->header_box = spin(grid, "Header (bytes)", 2, 0, 65534, 1);
            a->center_box = spin(grid, "Ontvangstfrequentie (MHz)", 3, 0, 100000, 1);
            gtk_spin_button_set_digits(GTK_SPIN_BUTTON(a->center_box), 6);
            gtk_widget_set_tooltip_text(a->center_box, "Stel hier de middenfrequentie van Linrad in voor de MHz-schaal. Dit stemt Linrad niet af.");
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->port_box), integer_setting(a, "linrad", "port", 1, 65535));
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->header_box), integer_setting(a, "linrad", "header_bytes", 0, 65534));
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->center_box), g_key_file_get_double(a->config, "linrad", "frequency", NULL)/1e6);
        } else if (source==SOURCE_FILE) {
            a->file_path=entry_field(grid,"IQ-bestand",0,a,"file","path");
            gtk_editable_set_editable(GTK_EDITABLE(a->file_path),FALSE);
            GtkWidget *browse=gtk_button_new_with_label("Bladeren…");
            gtk_grid_attach(GTK_GRID(grid),browse,2,0,1,1);
            g_signal_connect(browse,"clicked",G_CALLBACK(choose_iq_file),a);
            a->file_format=combo(grid,"Bestandsformaat",1,file_formats,G_N_ELEMENTS(file_formats),0);
            a->file_header=spin(grid,"Header overslaan (bytes)",2,0,100000000,1);
            a->file_center=spin(grid,"Ontvangstfrequentie (MHz)",3,0,1000000,1);
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
            const char *labels[] = {"Device-argumenten", "UHD-images (B210)", "Antenne"};
            for (int field=0; field<3; ++field) {
                GtkWidget *entry = entry_field(grid, labels[field], field, a, sources[source], keys[field]);
                a->hardware_entries[source][field] = entry;
                g_signal_connect(entry, "changed", G_CALLBACK(hardware_entry_changed), a);
            }
        }
    }
    a->frequency_box = spin(a->hardware, "SDR / IF (kHz)", 4, 1000, 6000000.0, 1);
    a->offset_box = spin(a->hardware, "LO-offset (kHz)", 5, -100000000, 100000000, 1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(a->offset_box), 3);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->offset_box), 0);
    gtk_widget_set_tooltip_text(a->offset_box, "Weergave = SDR-frequentie + LO-offset. Voor 436 MHz via 28 MHz: 408000 kHz.");
    a->step_box = spin(a->hardware, "Afstemstap (Hz)", 6, 1, 10000000, 100);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->step_box), 1000);
    a->frequency_calculation = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(a->frequency_calculation), 0);
    gtk_label_set_selectable(GTK_LABEL(a->frequency_calculation), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(a->frequency_calculation), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(a->frequency_calculation), 70);
    gtk_grid_attach(GTK_GRID(a->hardware), a->frequency_calculation, 0, 7, 2, 1);
    GtkWidget *hint = gtk_label_new("Wijzigingen gelden voor deze sessie. W in het hoofdvenster slaat ze op.");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_grid_attach(GTK_GRID(a->hardware), hint, 0, 8, 2, 1);
    g_signal_connect(a->config_dialog, "response", G_CALLBACK(config_response), a);
    g_signal_connect(a->config_dialog, "delete-event", G_CALLBACK(config_close), a);
}

static GtkWidget *settings_dialog(App *a, const char *title, GtkWidget **grid) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(a->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Sluiten", GTK_RESPONSE_CLOSE, NULL);
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

static void renderer_selection_changed(GtkWidget *widget, gpointer data) {
    App *a=data;
    selection_changed(widget, data);
    gtk_widget_set_sensitive(a->renderer_program, a->renderer == RENDER_CUSTOM);
    gtk_widget_set_sensitive(a->renderer_args, a->renderer == RENDER_CUSTOM);
    gtk_widget_set_sensitive(a->output_file, a->renderer == RENDER_FILE);
    scatter_refresh(a);
}

static gboolean apply_renderer(App *a) {
    if (a->running) return TRUE;
    const char *program = gtk_entry_get_text(GTK_ENTRY(a->renderer_program));
    const char *args = gtk_entry_get_text(GTK_ENTRY(a->renderer_args));
    const char *file = gtk_entry_get_text(GTK_ENTRY(a->output_file));
    GError *error=NULL; char **argv=NULL; int argc;
    if (a->renderer == RENDER_CUSTOM) {
        if (!*program) { gtk_label_set_text(GTK_LABEL(a->renderer_error), "Vul een programmanaam of pad in."); return FALSE; }
        if (*args && !g_shell_parse_argv(args, &argc, &argv, &error)) {
            gtk_label_set_text(GTK_LABEL(a->renderer_error), error->message); g_clear_error(&error); return FALSE;
        }
        g_strfreev(argv);
        if (a->scatter && !strstr(args, "{input}")) {
            gtk_label_set_text(GTK_LABEL(a->renderer_error), "Gebruik {input} in de argumenten voor de UDP-invoer van Scatterfix."); return FALSE;
        }
    }
    if (a->renderer == RENDER_FILE && !*file) {
        gtk_label_set_text(GTK_LABEL(a->renderer_error), "Vul een naam voor het MPEG-TS-bestand in."); return FALSE;
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

static void create_mode_renderer(App *a) {
    a->mode_dialog = settings_dialog(a, "Mode · DVB-instellingen", &a->controls);
    a->standard_box = combo(a->controls, "Mode", 0, standards, 3, a->standard);
    a->sr_box = combo(a->controls, "Symbolrate (sym/s)", 1, NULL, 0, -1);
    a->sr_label = gtk_grid_get_child_at(GTK_GRID(a->controls), 0, 1);
    a->fec_box = combo(a->controls, "FEC", 2, NULL, 0, -1);
    a->gi_box = combo(a->controls, "Guard interval", 3, guards, 4, a->gi);
    a->constellation_box = combo(a->controls, "Constellatie", 4, constellations, 2, a->constellation);
    a->renderer_dialog = settings_dialog(a, "Renderer · MPEG-TS-uitvoer", &a->renderer_panel);
    a->renderer_box = combo(a->renderer_panel, "Uitvoer", 0,
        (const char *[]) {"VLC (cvlc)", "VLC met bediening", "FFplay", "Eigen programma", "MPEG-TS-bestand"},
        G_N_ELEMENTS(renderers), a->renderer);
    a->renderer_program = entry_field(a->renderer_panel, "Programma / pad", 1, a, "paths", "custom_renderer");
    a->renderer_args = entry_field(a->renderer_panel, "Argumenten", 2, a, "renderer", "args");
    a->output_file = entry_field(a->renderer_panel, "MPEG-TS-bestand", 3, a, "renderer", "output_file");
    GtkWidget *hint = gtk_label_new("Eigen programma: MPEG-TS op stdin; {input} wordt - of de UDP-URL bij Scatterfix.\n"
        "Bestandsuitvoer schrijft rechtstreeks uit de decoder, zonder Scatterfix.\n"
        "Relatieve paden beginnen bij de INI-map. Bestaande bestanden worden niet overschreven.");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(hint), 64);
    gtk_grid_attach(GTK_GRID(a->renderer_panel), hint, 0, 4, 2, 1);
    a->renderer_error = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(a->renderer_error), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(a->renderer_error), 64);
    gtk_grid_attach(GTK_GRID(a->renderer_panel), a->renderer_error, 0, 5, 2, 1);
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
    if (!gtk_init_check(NULL, NULL)) fatal("Kan geen grafisch scherm openen. Gebruik --print-pipeline voor controle zonder GUI.");
    apply_receiver_theme();
    a->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(a->window), "Linrad DVB · PE1ITR");
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) monitor = gdk_display_get_monitor(display, 0);
    GdkRectangle work = {0, 0, 1600, 1000};
    if (monitor) gdk_monitor_get_workarea(monitor, &work);
    gtk_window_set_default_size(GTK_WINDOW(a->window), work.width*0.8, work.height*0.9);
    gtk_window_set_position(GTK_WINDOW(a->window), GTK_WIN_POS_CENTER);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(a->window), root);
    GtkWidget *menu = gtk_menu_bar_new();
    GtkWidget *config_item = gtk_menu_item_new_with_label("Input"), *info_item = gtk_menu_item_new_with_label("Info");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), config_item);
    GtkWidget *mode_item = gtk_menu_item_new_with_label("Mode"), *renderer_item = gtk_menu_item_new_with_label("Renderer");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mode_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), renderer_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), info_item);
    g_signal_connect(mode_item, "activate", G_CALLBACK(show_mode), a);
    g_signal_connect(renderer_item, "activate", G_CALLBACK(show_renderer), a);
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
    gtk_widget_set_tooltip_text(a->frequency_display, "Ontvangstfrequentie · ← / → afstemmen; bij Linrad stem je in Linrad af.");
    gtk_box_pack_start(GTK_BOX(top_bar), a->frequency_display, FALSE, FALSE, 8);
    a->gain_display = gtk_label_new("");
    gtk_widget_set_name(a->gain_display, "gain-display");
    gtk_widget_set_tooltip_text(a->gain_display, "↑ / ↓ gain hoger / lager · alleen bij directe SDR-invoer.");
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
    power_refresh(a);
    g_signal_connect(config_item, "activate", G_CALLBACK(show_config), a);
    g_signal_connect(info_item, "activate", G_CALLBACK(show_info), a);

    /* Two equally sized halves: waterfall + spectrum, then controls + logs. */
    a->dashboard = gtk_grid_new();
    gtk_grid_set_row_homogeneous(GTK_GRID(a->dashboard), TRUE);
    gtk_box_pack_start(GTK_BOX(root), a->dashboard, TRUE, TRUE, 0);
    GtkWidget *view = iq_view_create(&a->iq_view, a->config);
    a->filter_box = a->iq_view.filter_toggle;
    a->scatter_box = gtk_toggle_button_new_with_label("Scatterfix");
    gtk_widget_set_name(a->scatter_box,"spectrum-scatter");
    atk_object_set_name(gtk_widget_get_accessible(a->scatter_box),"Scatterfix aan/uit");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->scatter_box),a->scatter);
    gtk_box_pack_start(GTK_BOX(a->iq_view.filter_controls),a->scatter_box,FALSE,FALSE,0);
    gtk_widget_set_hexpand(view, TRUE); gtk_widget_set_vexpand(view, TRUE);
    gtk_grid_attach(GTK_GRID(a->dashboard), view, 0, 0, 1, 1);
    a->lower = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(a->lower), 6);
    gtk_widget_set_hexpand(a->lower, TRUE); gtk_widget_set_vexpand(a->lower, TRUE);
    GtkWidget *lower_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lower_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(lower_scroll), a->lower);
    gtk_grid_attach(GTK_GRID(a->dashboard), lower_scroll, 0, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(a->lower), decoder_view_create(&a->decoder_view), FALSE, FALSE, 0);
    create_mode_renderer(a);
    populate_rates(a); populate_fecs(a);
    a->status = gtk_label_new("");
    gtk_widget_set_no_show_all(a->status, TRUE);
    gtk_label_set_xalign(GTK_LABEL(a->status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(a->status), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(a->lower), a->status, FALSE, FALSE, 0);
    GtkWidget *diagnostics = gtk_expander_new("Opstartcommando / Log");
    GtkWidget *diagnostic_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(diagnostics), diagnostic_box);
    gtk_box_pack_start(GTK_BOX(a->lower), diagnostics, FALSE, FALSE, 0);
    a->preview = text_panel(diagnostic_box, "Opstartcommando", 55);
    a->log = text_panel(diagnostic_box, "Log", 80);
    create_config(a);
    source_changed(a->source_box, a);
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
    decoder_view_stop(&a->decoder_view);
    if (a->iq_view.fd >= 0) close(a->iq_view.fd);
    gtk_widget_destroy(a->mode_dialog);
    gtk_widget_destroy(a->renderer_dialog);
    gtk_widget_destroy(a->config_dialog);
    gtk_widget_destroy(a->window);
}

static int choice(const char *value, const char **choices, int n, const char *name) {
    for (int i = 0; i < n; ++i) if (!strcmp(value, choices[i])) return i;
    fatal("Ongeldige %s: %s", name, value);
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
    if (argc > 1 && !strcmp(argv[1], "--decoder-worker=dvbt"))
        return dvbt_backend_main(argc-1, argv+1);
    App a = {.standard = 0, .sr = 125000, .tune_fd = -1};
    char *source = NULL, *constellation = NULL;
    char *config = NULL, *standard = NULL, *fec = NULL, *gi = NULL, *iq_file = NULL;
    gboolean print = FALSE, run = FALSE, scatter = FALSE, no_scatter = FALSE;
    int sr = -1;
    GOptionEntry entries[] = {
        {"iq-file",0,0,G_OPTION_ARG_FILENAME,&iq_file,"IQ-bestand realtime afspelen (met JSON-metadata)","FILE"},
        {"constellation", 0, 0, G_OPTION_ARG_STRING, &constellation, "DVB-S2: QPSK of 8PSK", "MODULATION"},
        {"source", 0, 0, G_OPTION_ARG_STRING, &source, "linrad, hackrf, b210, sdriq of file", "SOURCE"},
        {"config", 'c', 0, G_OPTION_ARG_FILENAME, &config, "Eigen configuratiebestand", "FILE"},
        {"standard", 0, 0, G_OPTION_ARG_STRING, &standard, "DVB-S, DVB-S2 of DVB-T", "MODE"},
        {"sr", 0, 0, G_OPTION_ARG_INT, &sr, "Symbolrate (35000, 66000, 125000, 250000, 333000, 500000, 1000000, 2000000, 4000000, 5000000; DVB-T BW: 35000, 40000, 150000, 250000, 333000, 500000)", "RATE"},
        {"fec", 0, 0, G_OPTION_ARG_STRING, &fec, "FEC passend bij standaard/constellatie (bijv. 3/4)", "FEC"},
        {"gi", 0, 0, G_OPTION_ARG_STRING, &gi, "DVB-T: 1/32, 1/16, 1/8 of auto", "GI"},
        {"no-scatterfix", 0, 0, G_OPTION_ARG_NONE, &no_scatter, "Opgeslagen Scatterfix-keuze uitschakelen", NULL},
        {"scatterfix", 0, 0, G_OPTION_ARG_NONE, &scatter, "Scatterfix inschakelen", NULL},
        {"print-pipeline", 0, 0, G_OPTION_ARG_NONE, &print, "Pipeline tonen zonder processen te starten", NULL},
        {"run", 0, 0, G_OPTION_ARG_NONE, &run, "Zonder GUI starten; stoppen met Ctrl+C", NULL}, {NULL}
    };
    GOptionContext *context = g_option_context_new("- Linrad DVB ontvanger in C");
    g_option_context_add_main_entries(context, entries, NULL);
    GError *error = NULL;
    if (!g_option_context_parse(context, &argc, &argv, &error)) fatal("%s", error->message);
    if (argc != 1 || (print && run)) fatal("Ongeldige argumenten; zie --help");
    if (!config) {
        char *exe = g_file_read_link("/proc/self/exe", &error);
        if (!exe) fatal("Kan programmamap niet bepalen: %s", error->message);
        char *dir = g_path_get_dirname(exe); config = g_build_filename(dir, "..", "linrad-dvb.ini", NULL);
        g_free(exe); g_free(dir);
    }
    a.config_path = g_canonicalize_filename(config, NULL); a.directory = g_path_get_dirname(a.config_path);
    a.config = g_key_file_new();
    if (!g_key_file_load_from_file(a.config, a.config_path, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, &error)) fatal("%s: %s", a.config_path, error->message);
    source_defaults(&a);
    load_session(&a);
    if (source) a.source = choice(source, sources, G_N_ELEMENTS(sources), "input");
    if (iq_file && !select_iq_file(&a,iq_file,&error)) fatal("%s",error->message);
    if (standard) {
        int selected = choice(standard, standards, G_N_ELEMENTS(standards), "standaard");
        if (selected != a.standard) {
            a.standard = selected; a.constellation = 0; a.fec = 0;
            a.sr = a.standard == 2 ? 150000 : 125000;
        }
    }
    if (constellation) {
        a.constellation = choice(constellation, constellations, 2, "constellatie");
        if (!fec && !valid_fec(&a, a.fec)) a.fec = a.constellation ? 1 : 0;
    }
    g_free(constellation);
    if (a.constellation && a.standard != 1) fatal("8PSK vereist DVB-S2");
    if (fec) a.fec = choice(fec, fecs, G_N_ELEMENTS(fecs), "FEC");
    if (!valid_fec(&a, a.fec)) fatal("Ongeldige FEC voor de gekozen standaard/constellatie");
    if (gi) a.gi = choice(gi, guards, 4, "guard interval");
    if (sr != -1) a.sr = sr;
    const int *rates = a.standard == 2 ? terrestrial_rates : sat_rates;
    gboolean valid = FALSE;
    for (int i = 0; i < (a.standard == 2 ? (int)G_N_ELEMENTS(terrestrial_rates) : (int)G_N_ELEMENTS(sat_rates)); ++i)
        if (a.sr == rates[i]) valid = TRUE;
    if (!valid && !(a.source==SOURCE_FILE && a.sr>0 && a.sr<=100000000)) fatal("Ongeldige SR %d voor %s", a.sr, standards[a.standard]);
    if (scatter && no_scatter) fatal("Kies --scatterfix of --no-scatterfix");
    if (scatter) a.scatter = TRUE;
    if (no_scatter) a.scatter = FALSE;
    a.headless = run;
    g_free(source); g_free(iq_file);
    build_pipeline(&a);
    if (print) { char *text = describe(&a); puts(text); g_free(text); }
    else {
        signal(SIGPIPE, SIG_IGN);
        guint sigint = g_unix_signal_add(SIGINT, exit_signal, &a), sigterm = g_unix_signal_add(SIGTERM, exit_signal, &a);
        if (run) {
            a.loop = g_main_loop_new(NULL, FALSE); start_pipeline(&a);
            if (a.running) g_main_loop_run(a.loop);
            g_main_loop_unref(a.loop);
        } else gui(&a);
        g_source_remove(sigint); g_source_remove(sigterm);
    }
    clear_stages(&a); g_key_file_unref(a.config); g_free(a.config_path); g_free(a.directory);
    g_free(config); g_free(standard); g_free(fec); g_free(gi); g_option_context_free(context);
    return a.exit_code;
}
