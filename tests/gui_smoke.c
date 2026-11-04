/* Exercise actual GTK controls and pipeline callbacks on a headless display. */
#define main application_main
#include "../src/linrad_dvb.c"
#undef main
#include "../src/tune_control.h"

static void exercise_live_gain_locale(App *a) {
    /* GTK uses the user's numeric locale; the SDR helper parses in C locale. */
    locale_t decimal = newlocale(LC_NUMERIC_MASK, "nl_NL.UTF-8", (locale_t)0);
    if (!decimal) decimal = newlocale(LC_NUMERIC_MASK, "de_DE.UTF-8", (locale_t)0);
    if (!decimal) {
        g_printerr("Skipping comma-locale gain regression: no Dutch/German locale installed\n");
        return;
    }
    locale_t parser = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    g_assert_nonnull(parser);
    locale_t saved_locale = uselocale((locale_t)0);
    int source = a->source, saved_fd = a->tune_fd, fds[2];
    g_assert_false(a->running);
    g_assert_cmpint(pipe(fds), ==, 0);
    g_assert_true(g_unix_set_fd_nonblocking(fds[0], TRUE, NULL));
    a->running = TRUE; a->tune_fd = fds[1];
    GPtrArray *source_args = a->stages[0].args;
    const struct { int source; double initial, low, high, step; } cases[] = {
        {1, 40, 0, 116, 1}, {2, 30.5, 0, 76, 1},
        {3, -10, -20, 10, 10}, {SOURCE_AFEDRI, 14, -10, 35, 3},
        {SOURCE_FCDPP, 20, 0, 59, 1}, {SOURCE_LIME, 30, 0, 73, 1}
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(cases); ++i) {
        a->source = cases[i].source;
        const char *section = sources[a->source];
        double saved_gain = g_key_file_get_double(a->config, section, "gain", NULL);
        g_key_file_set_double(a->config, section, "gain", cases[i].initial);
        TuneControl control = {0};
        double received = cases[i].initial, frequency = 0, bandwidth = 0;
        for (int direction = 1; direction >= -1; direction -= 2) {
            uselocale(decimal);
            gain_step(a, direction);
            uselocale(parser);
            g_assert_cmpint(tune_control_read(&control, fds[0], 1, 0, &frequency,
                cases[i].low, cases[i].high, cases[i].source == 3 || cases[i].source == SOURCE_AFEDRI ? cases[i].step : 0,
                &received, 111111, &bandwidth), ==, 2);
            g_assert_cmpfloat(received, ==, cases[i].initial + (direction == 1 ? cases[i].step : 0));
            g_assert_cmpfloat(received, ==, g_key_file_get_double(a->config, section, "gain", NULL));
            g_assert_true(a->running);
            g_assert_true(source_args == a->stages[0].args);
        }
        g_key_file_set_double(a->config, section, "gain", saved_gain);
    }
    uselocale(saved_locale); freelocale(decimal); freelocale(parser);
    close(fds[0]); close(fds[1]);
    a->source = source; a->tune_fd = saved_fd; a->running = FALSE;
    tuning_refresh(a);
}

static void assert_no_tabs(GtkWidget *widget) {
    g_assert_false(GTK_IS_NOTEBOOK(widget));
    if (!GTK_IS_CONTAINER(widget)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (GList *p=children; p; p=p->next) assert_no_tabs(p->data);
    g_list_free(children);
}

static void exercise_record_controls(App *a) {
    g_assert_false(a->running);
    g_assert_false(gtk_widget_get_sensitive(a->record_button));
    g_assert_true(gtk_widget_get_parent(a->record_button) == gtk_widget_get_parent(a->power_button));
    GtkAllocation record_position, power_position;
    gtk_widget_get_allocation(a->record_button, &record_position);
    gtk_widget_get_allocation(a->power_button, &power_position);
    g_assert_cmpint(record_position.x + record_position.width, <=, power_position.x);
    int source = a->source, fds[2];
    g_assert_cmpint(pipe(fds), ==, 0);
    g_assert_true(g_unix_set_fd_nonblocking(fds[0], TRUE, NULL));
    a->running = TRUE; a->tune_fd = fds[1]; a->source = 0;
    record_refresh(a);
    g_assert_true(gtk_widget_get_sensitive(a->record_button));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->record_button), TRUE);
    char command[256] = {0};
    ssize_t n = read(fds[0], command, sizeof(command)-1);
    g_assert_cmpint(n, >, 0);
    g_assert_nonnull(strstr(command, " linrad-dvb-"));
    char *suffix = g_strdup_printf("-%.0f-%d.iq\n",
        g_key_file_get_double(a->config, "linrad", "frequency", NULL),
        g_key_file_get_integer(a->config, "linrad", "sample_rate", NULL));
    g_assert_true(g_str_has_suffix(command, suffix)); g_free(suffix);
    g_assert_true(a->record_pending);
    g_assert_false(gtk_widget_get_sensitive(a->record_button));
    const char *first = "unrelated log\n[iq-rec", *last = "ord] started test.iq\n";
    record_log(a, first, strlen(first));
    g_assert_false(a->recording);
    record_log(a, last, strlen(last));
    g_assert_true(a->recording);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->record_button)));
    g_assert_true(gtk_widget_get_sensitive(a->record_button));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->record_button), FALSE);
    memset(command, 0, sizeof(command));
    g_assert_cmpint(read(fds[0], command, sizeof(command)-1), >, 0);
    g_assert_cmpstr(command, ==, "record-stop\n");
    const char *stopped = "[iq-record] stopped test.iq\n";
    record_log(a, stopped, strlen(stopped));
    g_assert_false(a->recording); g_assert_false(a->record_pending);
    a->recording = TRUE; a->record_pending = TRUE;
    const char *failed = "[iq-record] error Disk full\n";
    record_log(a, failed, strlen(failed));
    g_assert_false(a->recording); g_assert_false(a->record_pending);
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->record_button)));
    a->source = SOURCE_FILE;
    record_refresh(a);
    g_assert_false(gtk_widget_get_sensitive(a->record_button));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->record_button), TRUE);
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->record_button)));
    g_assert_cmpint(read(fds[0], command, sizeof(command)), ==, -1);
    a->source = source; a->running = FALSE; a->tune_fd = -1;
    close(fds[0]); close(fds[1]); record_refresh(a);
    status(a, "");
}

static void exercise_lime(App *a) {
    int source=a->source;
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), SOURCE_LIME);
    g_assert_true(hardware_source(a));
    g_assert_null(a->hardware_entries[SOURCE_LIME][1]);
    double low, high;
    gtk_spin_button_get_range(GTK_SPIN_BUTTON(a->rate_box), &low, &high);
    g_assert_cmpfloat(low, ==, 100000); g_assert_cmpfloat(high, ==, 30720000);
    gtk_spin_button_get_range(GTK_SPIN_BUTTON(a->frequency_box), &low, &high);
    g_assert_cmpfloat(low, ==, 10000); g_assert_cmpfloat(high, ==, 3500000);
    gtk_entry_set_text(GTK_ENTRY(a->hardware_entries[SOURCE_LIME][0]), "serial=123");
    gtk_entry_set_text(GTK_ENTRY(a->hardware_entries[SOURCE_LIME][2]), "AUTO");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->rate_box), 30720000);
    tune_step(a,1); gain_step(a,1);
    g_assert_cmpfloat(g_key_file_get_double(a->config,"lime","frequency",NULL), ==, 437001000);
    g_assert_cmpfloat(g_key_file_get_double(a->config,"lime","gain",NULL), ==, 31);
    char *preview=describe(a);
    g_assert_nonnull(strstr(preview,"'--source' 'lime'"));
    g_assert_nonnull(strstr(preview,"'--sample-rate' '30720000'"));
    g_assert_nonnull(strstr(preview,"'--args' 'serial=123'"));
    g_free(preview);
    g_key_file_set_double(a->config,"lime","gain",73);
    gain_step(a,1);
    g_assert_cmpfloat(g_key_file_get_double(a->config,"lime","gain",NULL), ==, 73);
    g_key_file_set_double(a->config,"lime","frequency",3.5e9);
    tune_step(a,1);
    g_assert_cmpfloat(g_key_file_get_double(a->config,"lime","frequency",NULL), ==, 3.5e9);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), source);
}

static void exercise_rtlsdr(App *a) {
    int source = a->source;
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), SOURCE_RTLSDR);
    g_assert_true(hardware_source(a));
    g_assert_false(gtk_widget_get_visible(a->rate_box));
    g_assert_true(gtk_widget_get_visible(a->sdriq_rate_box));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(a->sdriq_rate_box)), ==, "2400000");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(a->hardware_entries[SOURCE_RTLSDR][0])), ==, "driver=rtlsdr");
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(a->sdriq_rate_box), "250000"));
    gtk_entry_set_text(GTK_ENTRY(a->hardware_entries[SOURCE_RTLSDR][0]), "driver=rtlsdr,serial=1007");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->frequency_box), 437000);
    tune_step(a, 1);
    gain_step(a, 1);
    g_assert_cmpfloat(g_key_file_get_double(a->config, "rtlsdr", "frequency", NULL), ==, 437001000);
    g_assert_cmpfloat(g_key_file_get_double(a->config, "rtlsdr", "gain", NULL), ==, 31);
    char *preview = describe(a);
    g_assert_nonnull(strstr(preview, "'--source' 'rtlsdr'"));
    g_assert_nonnull(strstr(preview, "'--sample-rate' '250000'"));
    g_assert_nonnull(strstr(preview, "'--args' 'driver=rtlsdr,serial=1007'"));
    g_free(preview);
    g_key_file_set_double(a->config, "rtlsdr", "gain", 49);
    gain_step(a, 1);
    g_assert_cmpfloat(g_key_file_get_double(a->config, "rtlsdr", "gain", NULL), ==, 49.6);
    gain_step(a, 1);
    g_assert_cmpfloat(g_key_file_get_double(a->config, "rtlsdr", "gain", NULL), ==, 49.6);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), 3);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(a->sdriq_rate_box), "111111"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), SOURCE_RTLSDR);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(a->sdriq_rate_box)), ==, "250000");
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), source);
}

static void exercise_funcube(App *a) {
    int source = a->source;
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), SOURCE_FCDPP);
    g_assert_true(hardware_source(a));
    g_assert_false(gtk_widget_get_visible(a->rate_box));
    g_assert_true(gtk_widget_get_visible(a->sdriq_rate_box));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(a->sdriq_rate_box)), ==, "192000");
    GtkTreeModel *rates = gtk_combo_box_get_model(GTK_COMBO_BOX(a->sdriq_rate_box));
    g_assert_cmpint(gtk_tree_model_iter_n_children(rates, NULL), ==, 1);
    gtk_entry_set_text(GTK_ENTRY(a->hardware_entries[SOURCE_FCDPP][0]), "lna=0,mixer=1");
    tune_step(a, 1); gain_step(a, 1);
    g_assert_cmpfloat(g_key_file_get_double(a->config, "fcdpp", "frequency", NULL), ==, 437001000);
    g_assert_cmpfloat(g_key_file_get_double(a->config, "fcdpp", "gain", NULL), ==, 21);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->gain_display)), ==, "IF gain 21 dB");
    char *preview = describe(a);
    g_assert_nonnull(strstr(preview, "'--source' 'fcdpp'"));
    g_assert_nonnull(strstr(preview, "'--sample-rate' '192000'"));
    g_assert_nonnull(strstr(preview, "'--args' 'lna=0,mixer=1'"));
    g_free(preview);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->frequency_box), 240000);
    tune_step(a, 1); /* Do not step into the tuner's frequency gap. */
    g_assert_cmpfloat(g_key_file_get_double(a->config, "fcdpp", "frequency", NULL), ==, 240000000);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->frequency_box), 437001);
    g_key_file_set_double(a->config, "fcdpp", "gain", 59);
    gain_step(a, 1);
    g_assert_cmpfloat(g_key_file_get_double(a->config, "fcdpp", "gain", NULL), ==, 59);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), 3);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(a->sdriq_rate_box), "111111"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), SOURCE_FCDPP);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(a->sdriq_rate_box)), ==, "192000");
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), source);
}

static void exercise_receiver_offset(App *a) {
    int source=a->source, standard=a->standard, sr=a->sr;
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box),SOURCE_FCDPP);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->standard_box),2);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->sr_box),2); /* DVB-T 40 kHz */
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->frequency_box),144600);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->receiver_offset_box),50);
    g_assert_null(receiver_offset_problem(a));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->frequency_display)),==,"144.650000 MHz");
    g_assert_cmpfloat(g_key_file_get_double(a->config,"fcdpp","frequency",NULL),==,144600000);
    monitor_tick(a);
    g_assert_cmpfloat(a->iq_view.center,==,144600000);
    g_assert_cmpfloat(a->iq_view.receiver_offset,==,50000);
    const char *screenshot=g_getenv("LINRAD_GUI_SCREENSHOT");
    if (screenshot) {
        GtkRequisition minimum,natural;
        gtk_widget_get_preferred_size(a->window,&minimum,&natural);
        GtkAllocation allocation={0,0,1280,900};
        gtk_widget_size_allocate(a->window,&allocation);
        cairo_surface_t *surface=cairo_image_surface_create(CAIRO_FORMAT_RGB24,
            gtk_widget_get_allocated_width(a->window),gtk_widget_get_allocated_height(a->window));
        cairo_t *cr=cairo_create(surface); gtk_widget_draw(a->window,cr);
        char *path=g_strconcat(screenshot,".offset.png",NULL);
        g_assert_cmpint(cairo_surface_write_to_png(surface,path),==,CAIRO_STATUS_SUCCESS);
        g_free(path); cairo_destroy(cr); cairo_surface_destroy(surface);
    }
    char *pipeline=describe(a);
    g_assert_nonnull(strstr(pipeline,"'--receiver-offset' '50000'"));
    g_assert_nonnull(strstr(pipeline,"'--channel-bandwidth' '40000'"));
    g_assert_nonnull(strstr(pipeline,"'--frequency' '144600000'")); g_free(pipeline);
    /* Arrow tuning sends hardware frequency while the display includes offset. */
    int fds[2]; g_assert_cmpint(pipe(fds),==,0);
    g_assert_true(g_unix_set_fd_nonblocking(fds[0],TRUE,NULL));
    a->running=TRUE; a->tune_fd=fds[1];
    for (int n=0;n<50;++n) tune_step(a,-1);
    TuneControl control={0}; double frequency=0,gain=0,bandwidth=0;
    g_assert_cmpint(tune_control_read(&control,fds[0],150000,1900000000,&frequency,
        0,59,1,&gain,192000,&bandwidth),==,1);
    g_assert_cmpfloat(frequency,==,144550000);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->frequency_display)),==,"144.600000 MHz");
    monitor_tick(a); g_assert_cmpfloat(a->iq_view.center,==,144550000);
    /* Recording stays raw: its RF metadata must NOT include receiver offset. */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->record_button),TRUE);
    char command[256]={0};
    g_assert_cmpint(read(fds[0],command,sizeof(command)-1),>,0);
    g_assert_true(g_str_has_prefix(command,"record 144550000 "));
    a->record_pending=FALSE; a->recording=FALSE;
    close(fds[0]); close(fds[1]); a->tune_fd=-1; a->running=FALSE; record_refresh(a);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->standard_box),0); /* 125 ksym/s no longer fits */
    g_assert_nonnull(receiver_offset_problem(a));
    start_pipeline(a); g_assert_false(a->running); a->exit_code=0;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->receiver_offset_box),0);
    g_assert_null(receiver_offset_problem(a));
    pipeline=describe(a); g_assert_null(strstr(pipeline,"--receiver-offset")); g_free(pipeline);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->standard_box),2);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->sr_box),2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->receiver_offset_box),-50);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->frequency_display)),==,"144.500000 MHz");
    /* Converter offset changes labels but does not change the digital shift. */
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->offset_box),100000);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->frequency_display)),==,"244.500000 MHz");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->offset_box),0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->frequency_box),437001);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->receiver_offset_box),50);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box),source);
    g_assert_cmpfloat(receiver_offset(a),==,0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->standard_box),standard);
    a->sr=sr; populate_rates(a); refresh_preview(a);
}

static void exercise_afedri(App *a) {
    int source = a->source;
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), SOURCE_AFEDRI);
    g_assert_true(hardware_source(a));
    g_assert_true(gtk_widget_get_visible(a->rate_box));
    g_assert_false(gtk_widget_get_visible(a->sdriq_rate_box));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(a->hardware_entries[SOURCE_AFEDRI][0])), ==, "192.168.0.118");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(a->hardware_entries[SOURCE_AFEDRI][1])), ==, "50005");
    gtk_entry_set_text(GTK_ENTRY(a->hardware_entries[SOURCE_AFEDRI][1]), "50006");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->frequency_box), 28001);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->rate_box), 200000);
    gain_step(a, 1);
    g_assert_cmpfloat(g_key_file_get_double(a->config, "afedri", "gain", NULL), ==, 17);
    g_assert_cmpfloat(g_key_file_get_double(a->config, "afedri", "frequency", NULL), ==, 28001000);
    char *preview = describe(a);
    g_assert_nonnull(strstr(preview, "'--ip' '192.168.0.118'"));
    g_assert_nonnull(strstr(preview, "'--port' '50006'"));
    g_free(preview);
    a->running = TRUE; a->afedri_receiving = FALSE;
    power_refresh(a);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(a->power_label)), "connecting Afedri"));
    const char *online = "[AFEDRI] Online: AFE822x (192.168.0.118:50005), TCP reply confirmed\n";
    record_log(a, online, strlen(online));
    g_assert_false(a->afedri_receiving);
    const char *iq = "[AFEDRI] Receiving IQ: single channel, s16, UDP confirmed\n";
    record_log(a, iq, 12);
    record_log(a, iq + 12, strlen(iq) - 12);
    g_assert_true(a->afedri_receiving);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(a->power_label)), "running"));
    a->running = FALSE; a->afedri_receiving = FALSE;
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), source);
    power_refresh(a);
}

static gboolean constellation_green_at(DecoderView *v, double re, double im) {
    int w=gtk_widget_get_allocated_width(v->area), h=gtk_widget_get_allocated_height(v->area);
    double size=MIN(w,h), scale=size/(4*.70710678);
    int x=floor((w-size)/2+size/2+re*scale), y=floor((h-size)/2+size/2-im*scale);
    cairo_surface_t *surface=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,w,h);
    cairo_t *cr=cairo_create(surface);
    gboolean handled=FALSE;
    g_signal_emit_by_name(v->area,"draw",cr,&handled);
    cairo_surface_flush(surface);
    uint32_t pixel;
    memcpy(&pixel,cairo_image_surface_get_data(surface)+y*cairo_image_surface_get_stride(surface)+4*x,4);
    gboolean green=((pixel>>8)&255) > ((pixel>>16)&255);
    cairo_destroy(cr); cairo_surface_destroy(surface);
    return green;
}

static void exercise_decoder_view(App *a) {
    DecoderView *v=&a->decoder_view;
    g_assert_true(decoder_view_start(v));
    int sender=socket(AF_INET,SOCK_DGRAM,0);
    g_assert_cmpint(sender,>=,0);
    struct sockaddr_in dest={.sin_family=AF_INET,.sin_port=htons(v->port),
        .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    DecoderStatus status={.magic=DECODER_MONITOR_MAGIC,.type=DECODER_STATUS,
        .locks=15,.snr_db=24,.pilot_lock=.97,.cfo_hz=12,.rms_dbfs=-20,.peak_dbfs=-8,
        .packets=100,.written_packets=98,.rs_ok=100,.service="Test <service>",
        .provider="PE1ITR",.modulation="DVB-T 2K QPSK",.fec="3/4",.guard="1/32"};
    DecoderBuffer queue={.magic=DECODER_MONITOR_MAGIC,.type=DECODER_BUFFER,.queued=256,.capacity=1024};
    DecoderConstellation points={.magic=DECODER_MONITOR_MAGIC,.type=DECODER_CONSTELLATION,.count=4,
        .iq={.707,.707,-.707,.707,-.707,-.707,.707,-.707}};
    g_strlcpy(v->selected_video_codec,"H.264/AVC",sizeof(v->selected_video_codec));
    g_strlcpy(v->selected_audio_codec,"AAC",sizeof(v->selected_audio_codec));
    g_assert_cmpint(sendto(sender,&status,sizeof(status),0,(struct sockaddr *)&dest,sizeof(dest)),==,sizeof(status));
    g_assert_cmpint(sendto(sender,&queue,sizeof(queue),0,(struct sockaddr *)&dest,sizeof(dest)),==,sizeof(queue));
    g_assert_cmpint(sendto(sender,&points,sizeof(points),0,(struct sockaddr *)&dest,sizeof(dest)),==,sizeof(points));
    decoder_view_tick(v,TRUE,"DVB-T");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->service)),==,"Test <service>");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->provider)),==,"PE1ITR");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->video_codec)),==,"H.264/AVC");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->audio_codec)),==,"AAC");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->buffer)),==,"25");
    g_assert_nonnull(strstr(gtk_label_get_label(GTK_LABEL(v->lamps[1])),"#36d866"));
    g_assert_cmpint(v->points.count,==,4);
    /* New frames add to the trail; expired points vanish, even while the
     * newest frame is retained during a brief pause in incoming telemetry. */
    points.iq[0]=.4; points.iq[1]=.4;
    sendto(sender,&points,sizeof(points),0,(struct sockaddr *)&dest,sizeof(dest));
    decoder_view_tick(v,TRUE,"DVB-T");
    g_assert_true(constellation_green_at(v,.707,.707));
    g_assert_true(constellation_green_at(v,.4,.4));
    v->history_time[0]-=DECODER_HISTORY_AGE+10000;
    g_assert_false(constellation_green_at(v,.707,.707));
    g_assert_true(constellation_green_at(v,.4,.4));
    status.rms_dbfs=NAN;
    sendto(sender,&status,sizeof(status),0,(struct sockaddr *)&dest,sizeof(dest));
    decoder_view_tick(v,TRUE,"DVB-T");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->rms)),==,"—");
    points.count=DECODER_POINTS+1;
    sendto(sender,&points,sizeof(points),0,(struct sockaddr *)&dest,sizeof(dest));
    sendto(sender,&status,7,0,(struct sockaddr *)&dest,sizeof(dest));
    decoder_view_tick(v,TRUE,"DVB-T");
    g_assert_cmpint(v->points.count,==,4);
    v->status_time-=11*G_USEC_PER_SEC; v->points_time-=11*G_USEC_PER_SEC; v->buffer_time-=11*G_USEC_PER_SEC;
    decoder_view_tick(v,TRUE,"DVB-T");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->buffer)),==,"—");
    g_assert_nonnull(strstr(gtk_label_get_label(GTK_LABEL(v->lamps[0])),"#888888"));
    g_assert_true(decoder_view_start(v));
    decoder_view_tick(v,TRUE,"DVB-T");
    g_assert_cmpint(v->points.count,==,0);
    for (unsigned i=0;i<DECODER_HISTORY_FRAMES;++i) g_assert_cmpint(v->history_time[i],==,0);
    g_assert_false(constellation_green_at(v,.4,.4));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->service)),==,"—");
    dest.sin_port=htons(v->port);
    status.satellite=2; status.constellation_order=8; status.mer_db=12.5;
    status.modcod=14; status.short_frame=1; status.pilots=1;
    status.lock_valid=DECODER_OFDM|DECODER_FEC|DECODER_TS;
    status.fec_good=42; status.fec_bad=2;
    g_strlcpy(v->selected_service,"PE1ORG",sizeof(v->selected_service));
    g_strlcpy(v->selected_provider,"0201",sizeof(v->selected_provider));
    g_strlcpy(v->selected_video_codec,"H.265/HEVC",sizeof(v->selected_video_codec));
    g_strlcpy(v->selected_audio_codec,"AAC",sizeof(v->selected_audio_codec));
    v->service_valid=TRUE;
    g_strlcpy(status.modulation,"DVB-S2 8PSK",sizeof(status.modulation));
    sendto(sender,&status,sizeof(status),0,(struct sockaddr *)&dest,sizeof(dest));
    decoder_view_tick(v,TRUE,"DVB-S2");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->service)),==,"PE1ORG");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->provider)),==,"0201");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->video_codec)),==,"H.265/HEVC");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->audio_codec)),==,"AAC");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->snr)),==,"12.5");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->guard)),==,"short");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->pilot)),==,"on");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->rs_ok)),==,"42");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->rs_errors)),==,"2");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(g_object_get_data(G_OBJECT(v->snr),"caption"))),==,"MER");
    g_assert_cmpuint(v->status.constellation_order,==,8);
    decoder_view_stop(v); close(sender);
    g_assert_cmpstr(v->selected_provider,==,"");
    decoder_view_tick(v,FALSE,"DVB-S2");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->video_codec)),==,"—");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->audio_codec)),==,"—");
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(v->state)),"stopped"));
    g_assert_cmpint(abs(gtk_widget_get_allocated_width(v->area)-gtk_widget_get_allocated_height(v->area)),<=,1);
}

static void exercise_file_selection(App *a) {
    int source=a->source, standard=a->standard, constellation=a->constellation;
    int fec=a->fec, gi=a->gi, sr=a->sr;
    char *path=g_build_filename(a->directory,"GUI recording.iq",NULL);
    char *metadata=g_strconcat(path,".json",NULL);
    char samples[4000]={0};
    GError *error=NULL;
    g_assert_true(g_file_set_contents(path,samples,sizeof(samples),&error));
    g_assert_true(g_file_set_contents(metadata,
        "{\"format\":\"cs16_le\",\"sample_rate_hz\":250000,\"standard\":\"DVB-T\","
        "\"bandwidth_hz\":40000,\"fec\":\"1/2\",\"gi\":\"1/32\",\"center_frequency_hz\":144600000}",-1,&error));
    g_assert_true(select_iq_file(a,path,&error));
    g_assert_no_error(error);
    file_selection_sync(a);
    g_assert_cmpint(a->source, ==, SOURCE_FILE);
    g_assert_cmpint(a->standard, ==, 2);
    g_assert_cmpint(a->sr, ==, 40000);
    g_assert_cmpfloat(gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->rate_box)), ==, 250000);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(a->file_path)), ==, path);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->power_label)), ==, "Play file");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->frequency_display)), ==, "144.600000 MHz");
    g_assert_false(gtk_widget_get_sensitive(a->frequency_box));
    g_assert_false(gtk_widget_get_sensitive(a->gain_display));
    g_assert_false(gtk_widget_get_sensitive(a->offset_box));
    g_assert_false(gtk_widget_get_sensitive(a->format_box));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(a->file_details)), "0.004 s"));
    char *command=describe(a);
    g_assert_nonnull(strstr(command,"iq_file_to_stdout"));
    g_assert_nonnull(strstr(command,"'--sr' '40000'")); g_free(command);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->rate_box),200000);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(a->file_details)), "0.005 s"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->file_format),1);
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(a->format_box)), ==, 1);
    g_assert_true(g_file_set_contents(metadata,"{broken",-1,&error));
    g_assert_false(select_iq_file(a,path,&error));
    g_clear_error(&error);
    g_assert_cmpint(a->source, ==, SOURCE_FILE);
    g_assert_cmpint(a->sr, ==, 40000);
    a->source=source; a->standard=standard; a->constellation=constellation;
    a->fec=fec; a->gi=gi; a->sr=sr;
    file_selection_sync(a);
    g_free(path); g_free(metadata);
}

static gboolean close_about(gpointer data) {
    gboolean *seen=data;
    GList *windows=gtk_window_list_toplevels();
    for (GList *p=windows;p;p=p->next) {
        if (GTK_IS_DIALOG(p->data) && gtk_widget_get_visible(p->data) &&
            !g_strcmp0(gtk_window_get_title(GTK_WINDOW(p->data)),"About · Linrad DVB")) {
            *seen=TRUE;
            GtkAllocation allocation;
            gtk_widget_get_allocation(p->data,&allocation);
            GdkMonitor *monitor=gdk_display_get_monitor_at_window(gtk_widget_get_display(p->data),gtk_widget_get_window(p->data));
            GdkRectangle work;
            g_assert_nonnull(monitor);
            gdk_monitor_get_workarea(monitor,&work);
            if (work.width<=0) work.width=1024;
            if (work.height<=0) work.height=768;
            const char *screenshot=g_getenv("LINRAD_GUI_SCREENSHOT");
            if (screenshot) {
                cairo_surface_t *surface=cairo_image_surface_create(CAIRO_FORMAT_RGB24,allocation.width,allocation.height);
                cairo_t *cr=cairo_create(surface);
                gtk_widget_draw(p->data,cr);
                char *dir=g_path_get_dirname(screenshot), *path=g_build_filename(dir,"about-smoke.png",NULL);
                g_assert_cmpint(cairo_surface_write_to_png(surface,path),==,CAIRO_STATUS_SUCCESS);
                g_free(dir); g_free(path); cairo_destroy(cr); cairo_surface_destroy(surface);
            }
            /* The command/credits reference scrolls inside the screen bounds. */
            g_assert_cmpint(allocation.height,<=,work.height);
            g_assert_cmpint(allocation.width,<=,work.width);
            gtk_dialog_response(GTK_DIALOG(p->data),GTK_RESPONSE_CLOSE);
        }
    }
    g_list_free(windows);
    return G_SOURCE_REMOVE;
}

static void exercise_menu_shortcuts(App *a) {
    g_assert_false(a->video_view.muted);
    for (unsigned upper=0;upper<2;++upper) {
        GdkEventKey event={.keyval=upper ? GDK_KEY_M : GDK_KEY_m};
        gboolean handled=FALSE;
        g_signal_emit_by_name(a->window,"key-press-event",&event,&handled);
        g_assert_true(handled);
        g_assert_cmpint(a->video_view.muted,==,!upper);
        g_assert_cmpint(usb_receiver_muted(a->usb_receiver),==,!upper);
        g_assert_cmpstr(gtk_frame_get_label(GTK_FRAME(a->video_view.container)),==,
            upper ? "Received video" : "Received video · Muted (M)");
        event.state=GDK_CONTROL_MASK;
        g_assert_false(tuning_key(a->window,&event,a));
        g_assert_cmpint(a->video_view.muted,==,!upper);
        g_assert_cmpint(usb_receiver_muted(a->usb_receiver),==,!upper);
    }
    const guint keys[]={GDK_KEY_i,GDK_KEY_m,GDK_KEY_r,GDK_KEY_t};
    GtkWidget *dialogs[]={a->config_dialog,a->mode_dialog,a->renderer_dialog,a->tci_dialog};
    gtk_window_set_focus(GTK_WINDOW(a->window),a->power_button);
    for (unsigned i=0;i<G_N_ELEMENTS(keys);++i) for (unsigned upper=0;upper<2;++upper) {
        GdkEventKey event={.keyval=upper ? gdk_keyval_to_upper(keys[i]) : keys[i]};
        /* Plain letters must not open menus (R is reserved for recovery). */
        tuning_key(a->window,&event,a);
        g_assert_false(gtk_widget_get_visible(dialogs[i]));
        event.state=GDK_MOD1_MASK;
        gboolean handled=FALSE;
        g_signal_emit_by_name(a->window,"key-press-event",&event,&handled);
        g_assert_true(handled);
        g_assert_true(gtk_widget_get_visible(dialogs[i]));
        gtk_dialog_response(GTK_DIALOG(dialogs[i]),GTK_RESPONSE_CLOSE);
        g_assert_false(gtk_widget_get_visible(dialogs[i]));
        event.state=GDK_CONTROL_MASK | GDK_MOD1_MASK;
        g_assert_false(tuning_key(a->window,&event,a));
        g_assert_false(gtk_widget_get_visible(dialogs[i]));
    }
    for (unsigned upper=0;upper<2;++upper) {
        gboolean seen=FALSE, handled=FALSE;
        GdkEventKey event={.keyval=upper ? GDK_KEY_A : GDK_KEY_a};
        g_assert_false(tuning_key(a->window,&event,a));
        event.state=GDK_MOD1_MASK;
        g_idle_add(close_about,&seen);
        g_signal_emit_by_name(a->window,"key-press-event",&event,&handled);
        g_assert_true(handled); g_assert_true(seen);
    }
}

static gboolean usb_preview(gpointer data) {
    App *a=data;
    /* Stop or panel changes can queue a resize in the same timer iteration. */
    GtkRequisition minimum, natural;
    GtkAllocation allocation;
    gtk_widget_get_preferred_size(a->window, &minimum, &natural);
    gtk_widget_get_allocation(a->window, &allocation);
    gtk_widget_size_allocate(a->window, &allocation);
    int width=gtk_widget_get_allocated_width(a->window), height=gtk_widget_get_allocated_height(a->window);
    cairo_surface_t *surface=cairo_image_surface_create(CAIRO_FORMAT_RGB24,width,height);
    cairo_t *cr=cairo_create(surface);
    gtk_widget_draw(a->window,cr);
    char *dir=g_path_get_dirname(g_getenv("LINRAD_GUI_SCREENSHOT"));
    char *path=g_build_filename(dir,a->iq_view.monitor_mode==MONITOR_FM ? "fm-smoke.png" : "usb-smoke.png",NULL);
    g_assert_cmpint(cairo_surface_write_to_png(surface,path),==,CAIRO_STATUS_SUCCESS);
    g_free(dir); g_free(path); cairo_destroy(cr); cairo_surface_destroy(surface);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->usb_toggle),FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->fm_toggle),FALSE);
    return G_SOURCE_REMOVE;
}

static void exercise_usb_clicks(App *a, gboolean enabled) {
    monitor_tick(a);
    double left=65, right=gtk_widget_get_allocated_width(a->iq_view.area)-40;
    double height=gtk_widget_get_allocated_height(a->iq_view.area);
    GdkEventButton click={.type=GDK_BUTTON_PRESS,.button=1,.y=height-45};
    double source_frequency=g_key_file_get_double(a->config,"linrad","frequency",NULL);
    for (unsigned i=0;i<2;++i) {
        double fraction=i ? .75 : .25;
        click.x=left+fraction*(right-left);
        double before=usb_receiver_frequency(a->usb_receiver);
        gboolean handled=FALSE;
        g_signal_emit_by_name(a->iq_view.area,"button-press-event",&click,&handled);
        g_assert_cmpint(handled,==,enabled);
        double expected=enabled ? round(a->iq_view.center+(fraction-.5)*a->iq_view.rate) : before;
        g_assert_cmpfloat(usb_receiver_frequency(a->usb_receiver),==,expected);
        g_assert_cmpfloat(a->iq_view.usb_frequency,==,expected);
        g_assert_cmpfloat(g_key_file_get_double(a->config,"linrad","frequency",NULL),==,source_frequency);
    }
    double before=usb_receiver_frequency(a->usb_receiver);
    click.x=left-1;
    g_assert_false(usb_spectrum_clicked(a->iq_view.area,&click,a));
    click.x=(left+right)/2; click.y=10; /* Waterfall is not the spectrum. */
    g_assert_false(usb_spectrum_clicked(a->iq_view.area,&click,a));
    click.y=height-10; /* Frequency axis. */
    g_assert_false(usb_spectrum_clicked(a->iq_view.area,&click,a));
    click.y=height-45; click.button=3;
    g_assert_false(usb_spectrum_clicked(a->iq_view.area,&click,a));
    click.button=1; click.state=GDK_CONTROL_MASK;
    g_assert_false(usb_spectrum_clicked(a->iq_view.area,&click,a));
    click.state=0; click.x=right;
    /* The 3 kHz USB passband must still fit within the captured spectrum. */
    g_assert_cmpint(usb_spectrum_clicked(a->iq_view.area,&click,a),==,enabled);
    g_assert_cmpfloat(usb_receiver_frequency(a->usb_receiver),==,before);
}

static void exercise_fm(App *a) {
    GPid source=a->stages[0].pid, decoder=a->stages[1].pid;
    double frequency=g_key_file_get_double(a->config,"linrad","frequency",NULL);
    double rate=g_key_file_get_double(a->config,"linrad","sample_rate",NULL);
    double bandwidth=g_key_file_get_double(a->config,"linrad","filter_bandwidth",NULL);
    GList *buttons=gtk_container_get_children(GTK_CONTAINER(a->iq_view.filter_controls));
    GList *fm=g_list_find(buttons,a->fm_toggle);
    g_assert_nonnull(fm); g_assert_nonnull(fm->prev); g_assert_true(fm->prev->data==a->usb_toggle);
    g_list_free(buttons);
    GdkEventKey key={.keyval=GDK_KEY_f,.state=GDK_CONTROL_MASK};
    g_assert_false(tuning_key(a->window,&key,a)); key.state=0;
    g_assert_true(tuning_key(a->window,&key,a));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->fm_toggle)));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->usb_toggle)));
    g_assert_cmpint(usb_receiver_mode(a->usb_receiver),==,MONITOR_FM);
    g_assert_cmpint(a->iq_view.monitor_mode,==,MONITOR_FM);
    double before=usb_receiver_frequency(a->usb_receiver);
    key.keyval=GDK_KEY_Page_Down; g_assert_true(tuning_key(a->window,&key,a));
    g_assert_cmpfloat(usb_receiver_frequency(a->usb_receiver),==,before+2500);
    key.keyval=GDK_KEY_Page_Up; g_assert_true(tuning_key(a->window,&key,a));
    g_assert_cmpfloat(usb_receiver_frequency(a->usb_receiver),==,before);
    exercise_usb_clicks(a,TRUE);
    key.keyval=GDK_KEY_m; g_assert_true(tuning_key(a->window,&key,a));
    g_assert_true(usb_receiver_muted(a->usb_receiver));
    key.keyval=GDK_KEY_u; g_assert_true(tuning_key(a->window,&key,a));
    g_assert_true(usb_receiver_running(a->usb_receiver));
    g_assert_cmpint(usb_receiver_mode(a->usb_receiver),==,MONITOR_USB);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->usb_toggle)));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->fm_toggle)));
    g_assert_true(usb_receiver_muted(a->usb_receiver));
    /* Reject FM if the IQ source is too narrow, keeping USB selected. */
    g_key_file_set_double(a->config,"linrad","sample_rate",8138);
    key.keyval=GDK_KEY_F; g_assert_true(tuning_key(a->window,&key,a));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->fm_toggle)));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->usb_toggle)));
    g_assert_cmpint(usb_receiver_mode(a->usb_receiver),==,MONITOR_USB);
    g_key_file_set_double(a->config,"linrad","sample_rate",rate);
    g_assert_true(tuning_key(a->window,&key,a));
    g_assert_cmpint(usb_receiver_mode(a->usb_receiver),==,MONITOR_FM);
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->usb_toggle)));
    /* DATA is exclusive too, retains mute and has symmetric 20 kHz tuning. */
    key.keyval=GDK_KEY_d; key.state=GDK_CONTROL_MASK;
    g_assert_false(tuning_key(a->window,&key,a)); key.state=0;
    g_assert_true(tuning_key(a->window,&key,a));
    g_assert_cmpint(usb_receiver_mode(a->usb_receiver),==,MONITOR_FM_DATA);
    g_assert_cmpint(a->iq_view.monitor_mode,==,MONITOR_FM_DATA);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->fm_data_toggle)));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->fm_toggle)));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->usb_toggle)));
    g_assert_true(usb_receiver_muted(a->usb_receiver));
    before=usb_receiver_frequency(a->usb_receiver);
    key.keyval=GDK_KEY_Page_Up; g_assert_true(tuning_key(a->window,&key,a));
    g_assert_cmpfloat(usb_receiver_frequency(a->usb_receiver),==,before-2500);
    key.keyval=GDK_KEY_Page_Down; g_assert_true(tuning_key(a->window,&key,a));
    g_assert_cmpfloat(usb_receiver_frequency(a->usb_receiver),==,before);
    exercise_usb_clicks(a,TRUE);
    key.keyval=GDK_KEY_D; g_assert_true(tuning_key(a->window,&key,a));
    g_assert_false(usb_receiver_running(a->usb_receiver));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->fm_data_toggle)));
    key.keyval=GDK_KEY_u; g_assert_true(tuning_key(a->window,&key,a));
    key.keyval=GDK_KEY_d; g_assert_true(tuning_key(a->window,&key,a));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->usb_toggle)));
    key.keyval=GDK_KEY_f; g_assert_true(tuning_key(a->window,&key,a));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->fm_data_toggle)));
    /* Voice FM fits a 16 kHz source, DATA must reject that transition. */
    g_key_file_set_double(a->config,"linrad","sample_rate",16000);
    key.keyval=GDK_KEY_d; g_assert_true(tuning_key(a->window,&key,a));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->fm_data_toggle)));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->fm_toggle)));
    g_assert_cmpint(usb_receiver_mode(a->usb_receiver),==,MONITOR_FM);
    g_key_file_set_double(a->config,"linrad","sample_rate",rate);
    key.keyval=GDK_KEY_M; g_assert_true(tuning_key(a->window,&key,a));
    g_assert_false(usb_receiver_muted(a->usb_receiver));
    key.keyval=GDK_KEY_F; g_assert_true(tuning_key(a->window,&key,a));
    g_assert_false(usb_receiver_running(a->usb_receiver));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->fm_toggle)));
    g_assert_cmpint(a->stages[0].pid,==,source); g_assert_cmpint(a->stages[1].pid,==,decoder);
    g_assert_cmpfloat(g_key_file_get_double(a->config,"linrad","frequency",NULL),==,frequency);
    g_assert_cmpfloat(g_key_file_get_double(a->config,"linrad","filter_bandwidth",NULL),==,bandwidth);
}

static void exercise_pcm_udp(App *a) {
    GPid source=a->stages[0].pid, decoder=a->stages[1].pid;
    g_assert_true(gtk_widget_is_ancestor(a->pcm_udp_enabled,a->tci_dialog));
    g_assert_false(usb_receiver_pcm_udp_enabled(a->usb_receiver));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->fm_data_toggle),TRUE);
    gtk_entry_set_text(GTK_ENTRY(a->pcm_udp_host),"invalid address");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->pcm_udp_enabled),TRUE);
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->pcm_udp_enabled)));
    g_assert_false(usb_receiver_pcm_udp_enabled(a->usb_receiver));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(a->status)),"PCM UDP"));
    g_assert_true(usb_receiver_running(a->usb_receiver));
    gtk_entry_set_text(GTK_ENTRY(a->pcm_udp_host),"127.0.0.1");
    /* Exercise a custom destination and an enable while already receiving. */
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->pcm_udp_port),7356);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->pcm_udp_enabled),TRUE);
    g_assert_true(usb_receiver_pcm_udp_enabled(a->usb_receiver));
    g_assert_false(gtk_widget_get_sensitive(a->pcm_udp_host));
    g_assert_false(gtk_widget_get_sensitive(a->pcm_udp_port));
    g_assert_true(g_key_file_get_boolean(a->config,"usb","pcm_udp_enabled",NULL));
    save_settings(a);
    App restored={.window=a->window,.config=g_key_file_new(),.usb_receiver=usb_receiver_new(NULL)};
    g_assert_nonnull(restored.usb_receiver);
    g_assert_true(g_key_file_load_from_file(restored.config,a->config_path,G_KEY_FILE_NONE,NULL));
    create_tci(&restored,NULL);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(restored.pcm_udp_enabled)));
    g_assert_true(usb_receiver_pcm_udp_enabled(restored.usb_receiver));
    g_assert_false(usb_receiver_running(restored.usb_receiver));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(restored.pcm_udp_host)),==,"127.0.0.1");
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(restored.pcm_udp_port)),==,7356);
    gtk_widget_destroy(restored.tci_dialog);
    usb_receiver_free(restored.usb_receiver); g_key_file_unref(restored.config);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->fm_toggle),TRUE);
    g_assert_true(usb_receiver_pcm_udp_enabled(a->usb_receiver));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->fm_toggle),FALSE);
    g_assert_false(usb_receiver_running(a->usb_receiver));
    g_assert_true(usb_receiver_pcm_udp_enabled(a->usb_receiver)); /* Armed for next start. */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->pcm_udp_enabled),FALSE);
    g_assert_false(usb_receiver_pcm_udp_enabled(a->usb_receiver));
    g_assert_true(gtk_widget_get_sensitive(a->pcm_udp_host));
    g_assert_true(gtk_widget_get_sensitive(a->pcm_udp_port));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->pcm_udp_port),7355);
    save_settings(a);
    g_assert_cmpint(a->stages[0].pid,==,source); g_assert_cmpint(a->stages[1].pid,==,decoder);
}

static void exercise_usb(App *a) {
    g_assert_nonnull(a->usb_receiver);
    g_assert_true(gtk_widget_get_parent(a->usb_toggle)==a->iq_view.filter_controls);
    g_assert_true(gtk_widget_is_ancestor(a->usb_port,a->tci_dialog));
    GList *buttons=gtk_container_get_children(GTK_CONTAINER(a->iq_view.filter_controls));
    GList *usb=g_list_find(buttons,a->usb_toggle);
    g_assert_nonnull(usb); g_assert_nonnull(usb->prev);
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(usb->prev->data)),==,"Teletext");
    g_list_free(buttons);
    GPid source=a->stages[0].pid, decoder=a->stages[1].pid;
    double frequency=g_key_file_get_double(a->config,"linrad","frequency",NULL);
    double bandwidth=g_key_file_get_double(a->config,"linrad","filter_bandwidth",NULL);
    g_key_file_set_string(a->config,"usb","audio_device","null");
    int fd=socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    g_assert_cmpint(bind(fd,(struct sockaddr *)&address,sizeof(address)),==,0);
    socklen_t length=sizeof(address);
    g_assert_cmpint(getsockname(fd,(struct sockaddr *)&address,&length),==,0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->usb_port),ntohs(address.sin_port));
    /* A busy port must only reject USB, leaving the running DVB pipeline intact. */
    GdkEventKey event={.keyval=GDK_KEY_u};
    g_assert_true(tuning_key(a->window,&event,a));
    g_assert_false(usb_receiver_running(a->usb_receiver));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->usb_toggle)));
    close(fd);
    for (unsigned upper=0;upper<2;++upper) {
        event.keyval=upper ? GDK_KEY_U : GDK_KEY_u;
        g_assert_true(tuning_key(a->window,&event,a));
        g_assert_cmpint(usb_receiver_running(a->usb_receiver),==,!upper);
        g_assert_cmpint(a->iq_view.usb_enabled,==,!upper);
        g_assert_cmpint(gtk_widget_get_sensitive(a->usb_port),==,upper);
        exercise_usb_clicks(a,!upper);
        event.state=GDK_CONTROL_MASK;
        g_assert_false(tuning_key(a->window,&event,a)); event.state=0;
        g_assert_true(a->running); g_assert_false(a->stopping);
        g_assert_cmpint(a->stages[0].pid,==,source);
        g_assert_cmpint(a->stages[1].pid,==,decoder);
    }
    g_assert_true(usb_receiver_tune(a->usb_receiver,frequency+1000));
    g_assert_false(usb_receiver_tune(a->usb_receiver,frequency+1e9));
    g_assert_cmpfloat(g_key_file_get_double(a->config,"linrad","frequency",NULL),==,frequency);
    g_assert_cmpfloat(g_key_file_get_double(a->config,"linrad","filter_bandwidth",NULL),==,bandwidth);
    GtkWidget *focus=gtk_window_get_focus(GTK_WINDOW(a->window));
    monitor_tick(a);
    const guint usb_keys[]={GDK_KEY_Page_Up,GDK_KEY_Page_Down,GDK_KEY_KP_Page_Up,GDK_KEY_KP_Page_Down};
    for (unsigned i=0;i<G_N_ELEMENTS(usb_keys);++i) {
        gtk_window_set_focus(GTK_WINDOW(a->window),i<2 ? a->power_button : a->usb_toggle);
        double before=usb_receiver_frequency(a->usb_receiver);
        event.keyval=usb_keys[i];
        g_assert_true(tuning_key(a->window,&event,a));
        g_assert_cmpfloat(usb_receiver_frequency(a->usb_receiver),==,before+(i%2 ? 100 : -100));
        event.state=GDK_CONTROL_MASK;
        g_assert_false(tuning_key(a->window,&event,a)); event.state=0;
        g_assert_cmpint(a->stages[0].pid,==,source);
        g_assert_cmpint(a->stages[1].pid,==,decoder);
        g_assert_cmpfloat(g_key_file_get_double(a->config,"linrad","frequency",NULL),==,frequency);
    }
    int source_index=a->source;
    a->source=1; /* Up/down must still send hardware gain with the USB button focused. */
    double gain=g_key_file_get_double(a->config,"hackrf","gain",NULL);
    g_key_file_set_double(a->config,"hackrf","gain",40);
    int control[2], saved_tune_fd=a->tune_fd;
    g_assert_cmpint(pipe(control),==,0); a->tune_fd=control[1];
    gtk_window_set_focus(GTK_WINDOW(a->window),a->usb_toggle);
    double before=usb_receiver_frequency(a->usb_receiver);
    event.keyval=GDK_KEY_Up;
    g_assert_true(tuning_key(a->window,&event,a));
    char command[64]={0};
    g_assert_cmpint(read(control[0],command,sizeof(command)-1),>,0);
    g_assert_cmpstr(command,==,"gain 41.0\n");
    event.keyval=GDK_KEY_Down;
    g_assert_true(tuning_key(a->window,&event,a));
    memset(command,0,sizeof(command));
    g_assert_cmpint(read(control[0],command,sizeof(command)-1),>,0);
    g_assert_cmpstr(command,==,"gain 40.0\n");
    g_assert_cmpfloat(usb_receiver_frequency(a->usb_receiver),==,before);
    close(control[0]); close(control[1]); a->tune_fd=saved_tune_fd;
    g_key_file_set_double(a->config,"hackrf","gain",gain);
    a->source=source_index;
    event.keyval=GDK_KEY_Up;
    g_assert_false(tuning_key(a->window,&event,a));
    g_assert_cmpfloat(usb_receiver_frequency(a->usb_receiver),==,before);
    monitor_tick(a);
    g_assert_cmpfloat(a->iq_view.usb_frequency,==,before);
    gtk_window_set_focus(GTK_WINDOW(a->window),focus);
    exercise_fm(a);
    exercise_pcm_udp(a);
    if (g_getenv("LINRAD_GUI_SCREENSHOT")) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->fm_toggle),TRUE);
        g_timeout_add(80,usb_preview,a);
    }
}

static void exercise_screenshot(App *a, guint key, const char *suffix) {
    /* Settle pending layout changes before sending a real window key event. */
    GtkRequisition minimum, natural;
    gtk_widget_get_preferred_size(a->window, &minimum, &natural);
    GtkAllocation allocation = {0, 0, 1280, 900};
    gtk_widget_size_allocate(a->window, &allocation);
    GdkEventKey event = {.keyval=key};
    gboolean handled = FALSE;
    g_signal_emit_by_name(a->window, "key-press-event", &event, &handled);
    g_assert_true(handled);
    const char *message = gtk_label_get_text(GTK_LABEL(a->status));
    g_assert_true(g_str_has_prefix(message, "Screenshot saved: "));
    const char *path = message + strlen("Screenshot saved: ");
    g_assert_true(g_str_has_suffix(path, suffix));
    char *name = g_path_get_basename(path);
    g_assert_true(g_regex_match_simple("^linrad-dvb-[0-9]{8}T[0-9]{6}\\.[0-9]{6}Z-", name, 0, 0));
    g_free(name);
    GdkPixbuf *png = gdk_pixbuf_new_from_file(path, NULL);
    g_assert_nonnull(png);
    int scale = gtk_widget_get_scale_factor(a->window);
    g_assert_cmpint(gdk_pixbuf_get_width(png), ==, allocation.width * scale);
    g_assert_cmpint(gdk_pixbuf_get_height(png), ==, allocation.height * scale);
    g_object_unref(png);
    event.state = GDK_CONTROL_MASK;
    g_assert_false(tuning_key(a->window, &event, a));
}

static void exercise_scatter_view(App *a) {
    g_assert_false(gtk_widget_get_visible(a->scatter_view.frame));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->scatter_box), TRUE);
    g_assert_false(gtk_widget_get_visible(a->programs.frame));
    g_assert_true(gtk_widget_get_visible(a->scatter_view.frame));
    g_assert_true(gtk_widget_get_parent(a->scatter_view.frame) ==
                  gtk_widget_get_parent(a->video_view.container));
    ScatterBackend sample = {0};
    g_mutex_init(&sample.mutex);
    const guint8 rgb[] = {255,0,0, 0,255,0, 0,0,255, 255,255,255};
    sample.image = g_bytes_new(rgb, sizeof(rgb));
    sample.width = sample.height = 2; sample.stride = 6;
    sample.status.phase = SF_IMAGE_READY; sample.status.progress_percent = 100;
    g_strlcpy(sample.status.provider_name, "PE1ITR", sizeof(sample.status.provider_name));
    g_strlcpy(sample.status.service_name, "Recovered test card", sizeof(sample.status.service_name));
    scatter_view_tick(&a->scatter_view, &sample);
    g_assert_nonnull(a->scatter_view.image);
    g_assert_cmpfloat(gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(a->scatter_view.progress)), ==, 1);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->scatter_view.provider)), ==, "Provider: PE1ITR");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->scatter_view.service)), ==, "Service: Recovered test card");
    /* The view retains its own image reference after the snapshot is freed. */
    g_bytes_unref(sample.image); g_mutex_clear(&sample.mutex);
    GtkAllocation allocation = {0, 0, 1600, 900};
    gtk_widget_size_allocate(a->window, &allocation);
    const char *screenshot = g_getenv("LINRAD_GUI_SCREENSHOT");
    if (screenshot) {
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1600, 900);
        cairo_t *cr = cairo_create(surface); gtk_widget_draw(a->window, cr);
        char *dir = g_path_get_dirname(screenshot), *path = g_build_filename(dir, "scatter-smoke.png", NULL);
        g_assert_cmpint(cairo_surface_write_to_png(surface, path), ==, CAIRO_STATUS_SUCCESS);
        g_free(path); g_free(dir); cairo_destroy(cr); cairo_surface_destroy(surface);
    }
    scatter_view_reset(&a->scatter_view);
    g_assert_null(a->scatter_view.image);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->scatter_box), FALSE);
    g_assert_false(gtk_widget_get_visible(a->scatter_view.frame));
    g_assert_true(gtk_widget_get_visible(a->programs.frame));
}

static void record_program(guint16 service, gpointer data) {
    guint16 *chosen = data; *chosen = service;
}

static void exercise_programs(App *a, const TsInfoSnapshot *snapshot) {
    ProgramView *v = &a->programs;
    guint16 chosen = 0;
    v->select = record_program; v->data = &chosen;
    program_view_update(v, snapshot, 1, TRUE);
    g_assert_cmpuint(v->count, ==, 2);
    g_assert_cmpuint(chosen, ==, 0); /* Discovery must not simulate a click. */
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(v->list), 1);
    gtk_list_box_select_row(GTK_LIST_BOX(v->list), row);
    g_assert_cmpuint(chosen, ==, 2);
    TsInfoSnapshot renamed = *snapshot;
    g_strlcpy(renamed.services[1].name, "Second program", sizeof(renamed.services[1].name));
    chosen = 0;
    program_view_update(v, &renamed, 2, TRUE);
    g_assert_cmpuint(chosen, ==, 0);
    row = gtk_list_box_get_selected_row(GTK_LIST_BOX(v->list));
    g_assert_cmpint(gtk_list_box_row_get_index(row), ==, 1);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(row)))), ==, "Second program");
    /* Information popups may select a different service without relabelling playback. */
    renamed.selected = 1;
    program_view_update(v, &renamed, 2, FALSE);
    g_assert_false(gtk_widget_is_sensitive(v->list));
    gtk_list_box_select_row(GTK_LIST_BOX(v->list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(v->list), 0));
    g_assert_cmpuint(chosen, ==, 0);
    program_view_update(v, &renamed, 2, TRUE);
    const char *screenshot = g_getenv("LINRAD_GUI_SCREENSHOT");
    if (screenshot) {
        GtkAllocation allocation = {0, 0, 1600, 900}; gtk_widget_size_allocate(a->window, &allocation);
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1600, 900);
        cairo_t *cr = cairo_create(surface); gtk_widget_draw(a->window, cr);
        char *dir = g_path_get_dirname(screenshot), *path = g_build_filename(dir, "programs-smoke.png", NULL);
        g_assert_cmpint(cairo_surface_write_to_png(surface, path), ==, CAIRO_STATUS_SUCCESS);
        g_free(path); g_free(dir); cairo_destroy(cr); cairo_surface_destroy(surface);
    }
    program_view_update(v, &(TsInfoSnapshot){0}, 0, FALSE);
    g_assert_cmpuint(v->count, ==, 0);
    g_assert_null(gtk_list_box_get_selected_row(GTK_LIST_BOX(v->list)));
    g_assert_cmpuint(chosen, ==, 0);
    v->select = select_program; v->data = a;
}

static void exercise_information(App *a) {
    TsInfoView *v=&a->information;
    g_assert_true(v->dialogs[0]!=v->dialogs[1]);
    for (int i=1;i>=0;i--) {
        g_assert_true(gtk_widget_get_parent(v->buttons[i])==gtk_widget_get_parent(a->scatter_box));
        g_assert_false(gtk_style_context_has_class(gtk_widget_get_style_context(v->buttons[i]),"information-ready"));
        gtk_button_clicked(GTK_BUTTON(v->buttons[i]));
        g_assert_true(gtk_widget_get_visible(v->dialogs[i]));
        g_assert_false(gtk_widget_get_visible(v->dialogs[!i]));
        g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(v->dialogs[i])),==,i ? "Teletext" : "EIT");
        assert_no_tabs(v->dialogs[i]);
        gtk_dialog_response(GTK_DIALOG(v->dialogs[i]),GTK_RESPONSE_CLOSE);
        g_assert_false(gtk_widget_get_visible(v->dialogs[i]));
    }
    char *path=g_build_filename(a->directory,"information.ts",NULL), *bytes=NULL; gsize size;
    g_assert_true(g_file_get_contents(path,&bytes,&size,NULL)); g_free(path);
    TsInfoBackend *backend=ts_info_backend_new();
    ts_info_backend_feed(backend,(guint8 *)bytes,size);
    gint64 deadline=g_get_monotonic_time()+3000000;
    do {
        v->next_tick=0; ts_info_view_tick(v,backend);
        if (v->snapshot.page) break;
        g_usleep(1000);
    } while (g_get_monotonic_time()<deadline);
    g_assert_nonnull(v->snapshot.page);
    exercise_programs(a, &v->snapshot);
    g_assert_cmpstr(v->snapshot.video_codec,==,"H.264/AVC");
    g_assert_true(v->snapshot.events[0].valid);
    for (unsigned i=0;i<2;i++)
        g_assert_true(gtk_style_context_has_class(gtk_widget_get_style_context(v->buttons[i]),"information-ready"));
    gtk_button_clicked(GTK_BUTTON(v->buttons[1]));
    gtk_button_clicked(GTK_BUTTON(v->buttons[0]));
    g_assert_true(gtk_widget_get_visible(v->dialogs[0]));
    g_assert_true(gtk_widget_get_visible(v->dialogs[1]));
    g_assert_true(gtk_widget_get_toplevel(v->eit)==v->dialogs[0]);
    g_assert_true(gtk_widget_get_toplevel(v->page)==v->dialogs[1]);
    gtk_dialog_response(GTK_DIALOG(v->dialogs[0]),GTK_RESPONSE_CLOSE);
    g_assert_true(gtk_widget_get_visible(v->dialogs[1]));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(v->reveal)));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->reveal),TRUE);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(v->reveal)));
    /* Only the broadcast raster belongs inside the 4:3 screen. */
    GtkWidget *aspect=gtk_widget_get_parent(v->page);
    g_assert_true(GTK_IS_ASPECT_FRAME(aspect));
    g_assert_true(gtk_bin_get_child(GTK_BIN(aspect))==v->page);
    g_assert_false(gtk_widget_is_ancestor(v->status[1],aspect));
    g_assert_false(gtk_widget_is_ancestor(v->reveal,aspect));
    GtkRequisition minimum,natural; gtk_widget_get_preferred_size(v->dialogs[1],&minimum,&natural);
    GtkAllocation allocation={0,0,720,640}; gtk_widget_size_allocate(v->dialogs[1],&allocation);
    gint page_x,page_y,status_x,status_y;
    g_assert_true(gtk_widget_translate_coordinates(v->page,v->dialogs[1],0,0,&page_x,&page_y));
    g_assert_true(gtk_widget_translate_coordinates(v->status[1],v->dialogs[1],0,0,&status_x,&status_y));
    g_assert_cmpint(status_y,>=,page_y+gtk_widget_get_allocated_height(v->page));
    g_assert_cmpint(abs(3*gtk_widget_get_allocated_width(v->page)-4*gtk_widget_get_allocated_height(v->page)),<=,4);
    const char *screenshot=g_getenv("LINRAD_GUI_SCREENSHOT");
    if (screenshot) {
        cairo_surface_t *surface=cairo_image_surface_create(CAIRO_FORMAT_RGB24,720,640);
        cairo_t *cr=cairo_create(surface); gtk_widget_draw(v->dialogs[1],cr);
        char *dir=g_path_get_dirname(screenshot), *file=g_build_filename(dir,"teletext-smoke.png",NULL);
        g_assert_cmpint(cairo_surface_write_to_png(surface,file),==,CAIRO_STATUS_SUCCESS);
        g_free(dir); g_free(file); cairo_destroy(cr); cairo_surface_destroy(surface);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(v->services[1]),"2");
    g_assert_cmpint(v->snapshot.selected,==,2);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(v->services[0])),==,"2");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(v->services[0]),"1");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(v->services[1])),==,"1");
    g_assert_null(v->snapshot.page); g_assert_false(v->snapshot.events[0].valid);
    for (unsigned i=0;i<2;i++)
        g_assert_false(gtk_style_context_has_class(gtk_widget_get_style_context(v->buttons[i]),"information-ready"));
    /* A reset during queued work cannot republish the old generation. */
    ts_info_backend_feed(backend,(guint8 *)bytes,size);
    ts_info_backend_reset(backend);
    ts_info_view_reset(v);
    g_assert_null(v->backend); g_assert_null(v->snapshot.page);
    ts_info_backend_finish(backend);
    deadline=g_get_monotonic_time()+3000000;
    while (!ts_info_backend_done(backend) && g_get_monotonic_time()<deadline) g_usleep(1000);
    g_assert_true(ts_info_backend_done(backend));
    TsInfoSnapshot after={0}; ts_info_backend_snapshot(backend,&after);
    g_assert_cmpuint(after.count,==,0); g_assert_null(after.page);
    ts_info_snapshot_clear(&after); ts_info_backend_free(backend); g_free(bytes);
    for (unsigned i=0;i<2;i++) {
        g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(v->services[i])),==,-1);
        gtk_dialog_response(GTK_DIALOG(v->dialogs[i]),GTK_RESPONSE_CLOSE);
    }
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(v->reveal)));
}

static void exercise_s2_automatic(App *a) {
    int standard=a->standard, sr=a->sr;
    char *backend=setting(a,"leandvb","backend");
    g_key_file_set_string(a->config,"leandvb","backend","embedded");
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(a->ldpc_latency_box)),==,0);
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(a->s2_acquisition_box)),==,0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->standard_box),1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->sr_box),2); // 125k
    char *command=describe(a);
    g_assert_nonnull(strstr(command,"'--fastlock'"));
    g_assert_nonnull(strstr(command,"'--ldpc-batch' '1'")); g_free(command);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->sr_box),G_N_ELEMENTS(sat_rates)-1); // 5M
    g_assert_cmpint(a->sr,==,5000000);
    command=describe(a);
    g_assert_null(strstr(command,"'--fastlock'"));
    g_assert_nonnull(strstr(command,"'--ldpc-batch' '32'")); g_free(command);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->ldpc_latency_box),1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->s2_acquisition_box),1);
    command=describe(a);
    g_assert_nonnull(strstr(command,"'--fastlock'"));
    g_assert_nonnull(strstr(command,"'--ldpc-batch' '1'")); g_free(command);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->ldpc_latency_box),0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->s2_acquisition_box),0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->sr_box),2);
    command=describe(a);
    g_assert_nonnull(strstr(command,"'--fastlock'"));
    g_assert_nonnull(strstr(command,"'--ldpc-batch' '1'")); g_free(command);
    g_key_file_set_string(a->config,"leandvb","backend",backend); g_free(backend);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->standard_box),standard);
    a->sr=sr; populate_rates(a); refresh_preview(a);
}

static gboolean exercise(gpointer data) {
    App *a = data;
    static int phase, ticks;
    g_assert_cmpint(++ticks, <, 100);
    if (phase == 0) {
        exercise_s2_automatic(a);
        exercise_information(a);
        exercise_scatter_view(a);
        exercise_decoder_view(a);
        exercise_file_selection(a);
        exercise_record_controls(a);
        exercise_afedri(a);
        exercise_rtlsdr(a);
        exercise_lime(a);
        exercise_funcube(a);
        exercise_receiver_offset(a);
        exercise_live_gain_locale(a);
        assert_no_tabs(a->window);
        g_assert_true(gtk_widget_get_parent(a->frequency_display) == gtk_widget_get_parent(a->power_button));
        g_assert_true(gtk_widget_get_parent(a->gain_display) == gtk_widget_get_parent(a->power_button));
        g_assert_false(gtk_widget_is_ancestor(a->standard_box, a->window));
        g_assert_false(gtk_widget_is_ancestor(a->renderer_box, a->window));
        g_assert_true(gtk_widget_get_parent(a->mode_summary) == gtk_widget_get_parent(a->power_button));
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->mode_summary)), ==, "DVB-S · Symb/s 125000 · FEC 1/2");
        g_assert_true(gtk_widget_get_parent(a->scatter_box) == gtk_widget_get_parent(a->filter_box));
        g_assert_cmpint(abs(2*gtk_widget_get_allocated_height(a->iq_view.area) -
                           gtk_widget_get_allocated_height(a->dashboard)), <=, 1);
        const char *screenshot = g_getenv("LINRAD_GUI_SCREENSHOT");
        if (screenshot) {
            /* Broadway has no browser client in this test, so allocate the
             * offscreen window explicitly before asking GTK to paint it. */
            GtkRequisition minimum, natural;
            gtk_widget_get_preferred_size(a->window, &minimum, &natural);
            GtkAllocation allocation = {0, 0, 1280, 900};
            gtk_widget_size_allocate(a->window, &allocation);
            cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
                gtk_widget_get_allocated_width(a->window), gtk_widget_get_allocated_height(a->window));
            cairo_t *cr = cairo_create(surface);
            gtk_widget_draw(a->window, cr);
            g_assert_cmpint(cairo_surface_write_to_png(surface, screenshot), ==, CAIRO_STATUS_SUCCESS);
            cairo_destroy(cr); cairo_surface_destroy(surface);
        }
        exercise_menu_shortcuts(a);
        show_config(NULL, a);
        g_assert_true(gtk_widget_get_visible(a->config_dialog));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->center_box), 437.125);
        config_response(GTK_DIALOG(a->config_dialog), GTK_RESPONSE_CLOSE, a);
        g_assert_false(gtk_widget_get_visible(a->config_dialog));
        g_assert_cmpfloat(g_key_file_get_double(a->config, "linrad", "frequency", NULL), ==, 437125000);
        monitor_tick(a);
        g_assert_cmpfloat(a->iq_view.center, ==, 437125000);
        g_assert_cmpfloat(a->iq_view.rate, ==, 1010526);
        exercise_screenshot(a, GDK_KEY_p, "-DVB-S-SR125000-437125kHz.png");
        show_mode(NULL, a);
        gtk_dialog_response(GTK_DIALOG(a->mode_dialog), GTK_RESPONSE_CLOSE);
        g_assert_false(gtk_widget_get_visible(a->mode_dialog));
        show_renderer(NULL, a);
        g_assert_true(gtk_widget_is_ancestor(a->video_view.area, a->window));
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->renderer_box), RENDER_EMBEDDED);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->udp_enabled), TRUE);
        gtk_entry_set_text(GTK_ENTRY(a->udp_host), "invalid address");
        g_assert_false(apply_renderer(a));
        gtk_entry_set_text(GTK_ENTRY(a->udp_host), "127.0.0.1");
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->udp_port), 1235);
        g_assert_true(apply_renderer(a));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->udp_port), 1234);
        g_assert_true(apply_renderer(a));
        g_assert_true(g_key_file_get_boolean(a->config, "renderer", "udp_enabled", NULL));
        char *embedded = describe(a);
        g_assert_nonnull(strstr(embedded, "embedded video panel + UDP MPEG-TS -> 127.0.0.1:1234"));
        g_free(embedded);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->renderer_box), RENDER_UDP);
        g_assert_true(apply_renderer(a));
        g_assert_true(gtk_widget_get_sensitive(a->udp_host));
        g_assert_false(gtk_widget_get_sensitive(a->udp_enabled));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->udp_enabled), FALSE);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->renderer_box), RENDER_FFPLAY);
        g_assert_true(apply_renderer(a));
        g_assert_false(gtk_widget_get_sensitive(a->renderer_program));
        g_assert_true(gtk_widget_get_sensitive(a->scatter_box));
        g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(a->renderer_box)), ==, RENDER_FFPLAY);
        save_settings(a);
        char *renderer = g_key_file_get_string(a->config, "session", "renderer", NULL);
        g_assert_cmpstr(renderer, ==, "ffplay"); g_free(renderer);
        char *ffplay = describe(a);
        g_assert_nonnull(strstr(ffplay, "'pipe:0'")); g_free(ffplay);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->renderer_box), RENDER_CUSTOM);
        gtk_entry_set_text(GTK_ENTRY(a->renderer_args), "'unfinished");
        g_assert_false(apply_renderer(a));
        gtk_entry_set_text(GTK_ENTRY(a->renderer_program), "test-player");
        gtk_entry_set_text(GTK_ENTRY(a->renderer_args), "--input={input}");
        g_assert_true(apply_renderer(a));
        char *custom = describe(a);
        g_assert_nonnull(strstr(custom, "'test-player' '--input=-'")); g_free(custom);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->renderer_box), RENDER_FILE);
        gtk_entry_set_text(GTK_ENTRY(a->output_file), "recording with spaces.ts");
        renderer_response(GTK_DIALOG(a->renderer_dialog), GTK_RESPONSE_CLOSE, a);
        g_assert_false(gtk_widget_get_visible(a->renderer_dialog));
        g_assert_cmpint(a->stage_count, ==, 2);
        g_assert_true(gtk_widget_get_sensitive(a->scatter_box));
        g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(a->output_file)), ==, "recording with spaces.ts");
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->renderer_box), RENDER_CVLC);
        /* Exercise telemetry reception and both Cairo displays with real bins. */
        g_assert_cmpint(a->iq_view.port, >, 0);
        int sender = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in destination = {.sin_family=AF_INET,
            .sin_port=htons(a->iq_view.port), .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
        IQMonitorFrame frame = {.magic=IQ_MONITOR_MAGIC, .rate=158730, .sequence=1};
        for (int i=0;i<IQ_MONITOR_BINS;++i) frame.power[i]=-145;
        frame.power[1200]=-80;
        g_assert_cmpint(sendto(sender,&frame,sizeof(frame),0,(struct sockaddr *)&destination,sizeof(destination)), ==, sizeof(frame));
        close(sender);
        a->iq_view.active=TRUE;
        iq_view_receive(&a->iq_view);
        g_assert_cmpint(a->iq_view.rows, ==, 1);
        g_assert_cmpfloat(a->iq_view.history[0][1200], ==, -80);
        GtkAllocation allocation = {0,0,800,500};
        gtk_widget_size_allocate(a->iq_view.area, &allocation);
        cairo_surface_t *surface=cairo_image_surface_create(CAIRO_FORMAT_RGB24,800,500);
        cairo_t *cr=cairo_create(surface);
        a->iq_view.bandwidth=40000; a->iq_view.enabled=TRUE;
        iq_view_draw(a->iq_view.area,cr,&a->iq_view);
        g_assert_cmpint(cairo_status(cr), ==, CAIRO_STATUS_SUCCESS);
        cairo_destroy(cr); cairo_surface_destroy(surface);
        a->iq_view.active=FALSE;
        g_assert_cmpint(a->sr, ==, 125000);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), 1);
        g_assert_true(gtk_widget_get_sensitive(a->frequency_box));
        g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(a->format_box)), ==, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->frequency_box), 1252000);
        g_assert_cmpfloat(g_key_file_get_double(a->config, "hackrf", "frequency", NULL), ==, 1252000000);
        while (g_key_file_get_double(a->config,"hackrf","gain",NULL) != 40)
            gain_step(a,g_key_file_get_double(a->config,"hackrf","gain",NULL) < 40 ? 1 : -1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->rate_box), 12000000);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), 2);
        g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(a->format_box)), ==, 0);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), 3);
        g_assert_true(gtk_widget_get_visible(a->sdriq_rate_box));
        g_assert_false(gtk_widget_get_visible(a->rate_box));
        g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(a->sdriq_rate_box)), ==, "111111");
        g_assert_cmpfloat(gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->frequency_box)), ==, 28000);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(a->sdriq_rate_box), "196078");
        while (g_key_file_get_double(a->config,"sdriq","gain",NULL) != -10)
            gain_step(a,g_key_file_get_double(a->config,"sdriq","gain",NULL) < -10 ? 1 : -1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->frequency_box), 28000.125);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->offset_box), 408000);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->step_box), 100);
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->frequency_display)), ==, "436.000125 MHz");
        gtk_window_set_focus(GTK_WINDOW(a->window), a->power_button);
        GdkEventKey event = {.keyval = GDK_KEY_Right};
        g_assert_true(tuning_key(a->window, &event, a));
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->frequency_display)), ==, "436.000225 MHz");
        event.keyval = GDK_KEY_Left;
        g_assert_true(tuning_key(a->window, &event, a));
        gtk_window_set_focus(GTK_WINDOW(a->window), a->power_button);
        gboolean handled = FALSE;
        g_signal_emit_by_name(a->window, "key-press-event", &event, &handled);
        g_assert_true(handled);
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->frequency_display)), ==, "436.000025 MHz");
        event.keyval = GDK_KEY_Right;
        g_signal_emit_by_name(a->window, "key-press-event", &event, &handled);
        g_assert_true(handled);
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->frequency_display)), ==, "436.000125 MHz");
        event.state = GDK_CONTROL_MASK;
        g_assert_false(tuning_key(a->window, &event, a));
        event.state = 0;
        /* A live retune must send hardware IF, not the displayed RF frequency,
         * and must leave all running Stage objects intact. */
        int control_pipe[2]; g_assert_cmpint(pipe(control_pipe), ==, 0);
        a->tune_fd = control_pipe[1]; a->running = TRUE;
        GPtrArray *source_args = a->stages[0].args;
        tune_step(a, 1);
        g_assert_true(source_args == a->stages[0].args);
        char command[64] = {0};
        g_assert_cmpint(read(control_pipe[0], command, sizeof(command)-1), >, 0);
        g_assert_cmpstr(command, ==, "28000225\n");
        event.keyval = GDK_KEY_Down;
        g_signal_emit_by_name(a->window, "key-press-event", &event, &handled);
        g_assert_true(handled);
        memset(command, 0, sizeof(command));
        g_assert_cmpint(read(control_pipe[0], command, sizeof(command)-1), >, 0);
        g_assert_cmpstr(command, ==, "gain -20.0\n");
        g_assert_cmpfloat(g_key_file_get_double(a->config, "sdriq", "gain", NULL), ==, -20);
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->gain_display)), ==, "Gain -20 dB");
        gain_step(a, -1); /* Limit: no command and no config change. */
        g_assert_cmpfloat(g_key_file_get_double(a->config, "sdriq", "gain", NULL), ==, -20);
        event.keyval = GDK_KEY_Up;
        g_signal_emit_by_name(a->window, "key-press-event", &event, &handled);
        g_assert_true(handled);
        memset(command, 0, sizeof(command));
        g_assert_cmpint(read(control_pipe[0], command, sizeof(command)-1), >, 0);
        g_assert_cmpstr(command, ==, "gain -10.0\n");
        g_assert_true(source_args == a->stages[0].args);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->filter_box), TRUE);
        memset(command, 0, sizeof(command));
        g_assert_cmpint(read(control_pipe[0], command, sizeof(command)-1), >, 0);
        g_assert_cmpstr(command, ==, "bandwidth 50000\n");
        event.keyval = GDK_KEY_bracketleft;
        g_assert_true(tuning_key(a->window, &event, a));
        memset(command, 0, sizeof(command));
        g_assert_cmpint(read(control_pipe[0], command, sizeof(command)-1), >, 0);
        g_assert_cmpstr(command, ==, "bandwidth 47500\n");
        g_assert_cmpint(g_key_file_get_integer(a->config, "sdriq", "sample_rate", NULL), ==, 196078);
        g_assert_true(source_args == a->stages[0].args);
        event.keyval = GDK_KEY_bracketright;
        g_assert_true(tuning_key(a->window, &event, a));
        memset(command, 0, sizeof(command));
        g_assert_cmpint(read(control_pipe[0], command, sizeof(command)-1), >, 0);
        g_assert_cmpstr(command, ==, "bandwidth 49875\n");
        event.keyval = GDK_KEY_w;
        g_assert_true(tuning_key(a->window, &event, a));
        g_assert_true(a->running);
        g_assert_true(source_args == a->stages[0].args);
        close(control_pipe[0]); close(control_pipe[1]);
        a->tune_fd = -1; a->running = FALSE;
        tune_step(a, -1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->frequency_box), 30000);
        tune_step(a, 1);
        g_assert_cmpfloat(g_key_file_get_double(a->config, "sdriq", "frequency", NULL), ==, 30000000);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->frequency_box), 28000.125);
        g_assert_cmpint(g_key_file_get_integer(a->config, "sdriq", "sample_rate", NULL), ==, 196078);
        g_assert_cmpfloat(g_key_file_get_double(a->config, "sdriq", "frequency", NULL), ==, 28000125);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), 1);
        g_assert_true(gtk_widget_get_visible(a->rate_box));
        g_assert_false(gtk_widget_get_visible(a->sdriq_rate_box));
        g_assert_cmpfloat(gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->frequency_box)), ==, 1252000);
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->gain_display)), ==, "Gain 40 dB");
        g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->rate_box)), ==, 12000000);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->standard_box), 1);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->sr_box), 9);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->fec_box), 2);
        g_assert_cmpint(a->sr, ==, 5000000);
        char *pipeline = describe(a);
        g_assert_nonnull(strstr(pipeline, "0x80")); g_free(pipeline);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->constellation_box), 1);
        g_assert_cmpint(a->fec, ==, 2);
        pipeline = describe(a);
        g_assert_nonnull(strstr(pipeline, "0x4000")); g_free(pipeline);
        g_assert_false(gtk_combo_box_set_active_id(GTK_COMBO_BOX(a->fec_box), "0"));
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->constellation_box), 0);
        g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(a->fec_box), "0"));
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->constellation_box), 1);
        g_assert_cmpint(a->fec, ==, 1); /* 1/2 is unavailable for 8PSK. */
        g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(a->fec_box), "10"));
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), 0);
        g_assert_false(gtk_widget_get_sensitive(a->frequency_box));
        g_assert_false(gtk_widget_get_sensitive(a->gi_box));
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->standard_box), 2);
        g_assert_cmpint(a->sr, ==, 150000);
        g_assert_false(gtk_widget_get_sensitive(a->constellation_box));
        g_assert_cmpint(a->fec, ==, 0); /* DVB-T cannot retain 9/10. */
        g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_combo_box_get_model(GTK_COMBO_BOX(a->fec_box)),NULL), ==, 5);
        g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(a->fec_box), "8"));
        pipeline=describe(a);
        g_assert_nonnull(strstr(pipeline,"'--fec' '5/6'")); g_free(pipeline);
        g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(a->fec_box), "11"));
        pipeline=describe(a);
        g_assert_nonnull(strstr(pipeline,"'--fec' '7/8'")); g_free(pipeline);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->standard_box), 1);
        g_assert_false(gtk_combo_box_set_active_id(GTK_COMBO_BOX(a->fec_box), "11"));
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->standard_box), 2);
        g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(a->fec_box), "0"));
        g_assert_true(gtk_widget_get_sensitive(a->gi_box));
        g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_combo_box_get_model(GTK_COMBO_BOX(a->sr_box)),NULL), ==, 6);
        for (int index=4;index<=5;++index) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(a->sr_box),index);
            g_assert_cmpint(a->sr, ==, index==4 ? 333000 : 500000);
            pipeline=describe(a);
            g_assert_nonnull(strstr(pipeline,index==4 ? "'--sr' '333000'" : "'--sr' '500000'"));
            g_free(pipeline);
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), 3);
        g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(a->sdriq_rate_box)), ==, "196078");
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->gain_display)), ==, "Gain -10 dB");
        g_assert_cmpfloat(gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->offset_box)), ==, 408000);
        g_assert_cmpfloat(gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->step_box)), ==, 100);
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->frequency_display)), ==, "436.000125 MHz");
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->sr_box), 2);
        g_assert_cmpint(a->sr, ==, 40000);
        g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(a->mode_summary)), "DVB-T · BW 40000 Hz · FEC"));
        pipeline = describe(a);
        g_assert_nonnull(strstr(pipeline, "'--sr' '40000'"));
        g_assert_nonnull(strstr(pipeline, "'--sample-rate' '196078'")); g_free(pipeline);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->renderer_box), RENDER_FILE);
        /* W must persist display adjustments as well as receiver settings. */
        exercise_screenshot(a, GDK_KEY_P, "-DVB-T-BW40000-436000kHz.png");
        gtk_button_clicked(GTK_BUTTON(a->iq_view.level_buttons[0]));
        gtk_button_clicked(GTK_BUTTON(a->iq_view.level_buttons[3]));
        double saved_floor=a->iq_view.floor, saved_span=a->iq_view.span;
        event.keyval = GDK_KEY_W;
        g_signal_emit_by_name(a->window, "key-press-event", &event, &handled);
        g_assert_true(handled);
        g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(a->status)), "Settings saved"));
        App restored = {.tune_fd = -1};
        restored.config = g_key_file_new();
        g_assert_true(g_key_file_load_from_file(restored.config, a->config_path, G_KEY_FILE_NONE, NULL));
        load_session(&restored);
        g_assert_cmpint(g_key_file_get_integer(restored.config, "afedri", "port", NULL), ==, 50006);
        g_assert_cmpint(g_key_file_get_integer(restored.config, "afedri", "sample_rate", NULL), ==, 200000);
        g_assert_cmpint(g_key_file_get_integer(restored.config, "rtlsdr", "sample_rate", NULL), ==, 250000);
        g_assert_cmpfloat(g_key_file_get_double(restored.config, "rtlsdr", "gain", NULL), ==, 49.6);
        g_assert_cmpfloat(g_key_file_get_double(restored.config, "rtlsdr", "frequency", NULL), ==, 437001000);
        g_assert_cmpint(g_key_file_get_integer(restored.config, "fcdpp", "sample_rate", NULL), ==, 192000);
        g_assert_cmpfloat(g_key_file_get_double(restored.config, "fcdpp", "gain", NULL), ==, 59);
        g_assert_cmpfloat(g_key_file_get_double(restored.config, "fcdpp", "frequency", NULL), ==, 437001000);
        g_assert_cmpfloat(g_key_file_get_double(restored.config, "fcdpp", "receiver_offset", NULL), ==, 50000);
        g_assert_cmpint(restored.source, ==, 3);
        g_assert_cmpint(restored.standard, ==, 2);
        g_assert_cmpint(restored.sr, ==, 40000);
        g_assert_cmpint(restored.constellation, ==, 0);
        g_assert_cmpint(restored.renderer, ==, RENDER_FILE);
        char *saved_file = setting(&restored, "renderer", "output_file");
        g_assert_cmpstr(saved_file, ==, "recording with spaces.ts"); g_free(saved_file);
        g_assert_true(g_key_file_get_boolean(restored.config, "sdriq", "filter_enabled", NULL));
        g_assert_cmpint(g_key_file_get_integer(restored.config, "sdriq", "filter_bandwidth", NULL), ==, 49875);
        g_assert_cmpfloat(g_key_file_get_double(restored.config, "sdriq", "lo_offset", NULL), ==, 408000000);
        g_assert_cmpfloat(g_key_file_get_double(restored.config, "sdriq", "frequency", NULL), ==, 28000125);
        g_assert_cmpfloat(g_key_file_get_double(restored.config, "sdriq", "gain", NULL), ==, -10);
        g_assert_cmpfloat(g_key_file_get_double(restored.config, "display", "floor_dbfs_hz", NULL), ==, saved_floor);
        g_assert_cmpfloat(g_key_file_get_double(restored.config, "display", "range_db", NULL), ==, saved_span);
        GtkWidget *restored_spectrum=iq_view_create(&restored.iq_view,restored.config);
        g_object_ref_sink(restored_spectrum);
        g_assert_cmpfloat(restored.iq_view.floor, ==, saved_floor);
        g_assert_cmpfloat(restored.iq_view.span, ==, saved_span);
        close(restored.iq_view.fd);
        gtk_widget_destroy(restored_spectrum); g_object_unref(restored_spectrum);
        /* Missing or malformed display settings leave older INIs usable. */
        g_key_file_remove_group(restored.config,"display",NULL);
        g_assert_cmpfloat(iq_view_saved_level(restored.config,"floor_dbfs_hz",-160,-200,-10), ==, -160);
        g_key_file_set_string(restored.config,"display","range_db","nan");
        g_assert_cmpfloat(iq_view_saved_level(restored.config,"range_db",100,10,160), ==, 100);
        g_key_file_set_double(restored.config,"display","range_db",0);
        g_assert_cmpfloat(iq_view_saved_level(restored.config,"range_db",100,10,160), ==, 100);
        g_key_file_unref(restored.config);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->renderer_box), RENDER_CVLC);
        char *original_path = a->config_path;
        a->config_path = g_build_filename(original_path, "cannot-write.ini", NULL);
        save_settings(a);
        g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(a->status)), "Save failed"));
        g_free(a->config_path); a->config_path = original_path;

        gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box), 0);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->sr_box), 1);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->fec_box), 1);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->gi_box), 2);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->scatter_box), TRUE);
        g_assert_cmpint(a->sr, ==, 250000);
        g_assert_cmpint(a->fec, ==, 1);
        g_assert_cmpint(a->gi, ==, 2);
        g_assert_true(a->scatter);
        g_assert_true(gtk_widget_get_visible(a->scatter_view.frame));
        gtk_button_clicked(GTK_BUTTON(a->power_button));
        g_assert_true(a->running);
        g_assert_false(gtk_widget_get_sensitive(a->controls));
        g_assert_false(gtk_widget_get_sensitive(a->hardware));
        g_assert_false(gtk_widget_get_sensitive(a->renderer_panel));
        g_assert_true(gtk_widget_is_sensitive(a->scatter_box));
        GPid decoder_pid = a->stages[1].pid;
        exercise_usb(a);
        ScatterBackend *backend = a->scatter_backend;
        for (unsigned upper = 0; upper < 2; ++upper) {
            g_mutex_lock(&backend->mutex);
            guint64 generation = backend->generation;
            g_mutex_unlock(&backend->mutex);
            GdkEventKey reset = {.keyval=upper ? GDK_KEY_R : GDK_KEY_r};
            gboolean handled = FALSE;
            g_signal_emit_by_name(a->window, "key-press-event", &reset, &handled);
            g_assert_true(handled);
            g_assert_true(a->running);
            g_assert_true(a->scatter);
            g_assert_null(a->scatter_view.image);
            g_assert_false(gtk_widget_get_visible(a->renderer_dialog));
            g_assert_true(a->scatter_backend == backend);
            g_assert_cmpint(a->stages[1].pid, ==, decoder_pid);
            reset.state = GDK_CONTROL_MASK;
            g_assert_false(tuning_key(a->window, &reset, a));
            g_mutex_lock(&backend->mutex);
            g_assert_cmpuint(backend->generation, ==, generation + 1);
            g_assert_true(backend->enabled);
            g_mutex_unlock(&backend->mutex);
        }
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->scatter_box), FALSE);
        g_assert_false(a->scatter);
        g_assert_false(gtk_widget_get_visible(a->scatter_view.frame));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->scatter_box), TRUE);
        g_assert_true(a->scatter);
        g_assert_true(gtk_widget_get_visible(a->scatter_view.frame));
        g_assert_cmpint(a->stages[1].pid, ==, decoder_pid);
        g_assert_true(a->scatter_backend == backend);
        g_assert_true(gtk_widget_is_sensitive(a->filter_box));
        g_assert_true(gtk_widget_get_sensitive(a->power_button));
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->power_label)), ==, "Stop SDR · running");
        g_assert_true(gtk_style_context_has_class(gtk_widget_get_style_context(a->power_button), "receiver-running"));
        ++phase;
    } else if (phase == 1) {
        g_assert_cmpint(a->exit_code, ==, 0);
        exercise_screenshot(a, GDK_KEY_P, "-DVB-T-BW250000-437125kHz.png");
        gtk_button_clicked(GTK_BUTTON(a->power_button));
        g_assert_false(gtk_widget_get_sensitive(a->power_button));
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->power_label)), ==, "Stopping SDR…");
        ++phase;
    } else if (phase == 2 && !a->running) {
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->power_label)), ==, "Start SDR");
        g_assert_true(gtk_style_context_has_class(gtk_widget_get_style_context(a->power_button), "receiver-off"));
        g_assert_true(gtk_widget_get_sensitive(a->controls));
        g_assert_true(gtk_widget_is_sensitive(a->scatter_box));
        g_assert_true(gtk_widget_get_sensitive(a->power_button));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->scatter_box), FALSE);
        g_assert_false(gtk_widget_get_visible(a->scatter_view.frame));
        g_assert_null(a->scatter_backend);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->standard_box), 1);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->renderer_box), RENDER_EMBEDDED);
        g_assert_true(apply_renderer(a));
        g_assert_cmpint(a->sr, ==, 250000);
        g_assert_false(gtk_widget_get_sensitive(a->gi_box));
        gtk_button_clicked(GTK_BUTTON(a->power_button));
        g_assert_true(a->running); ++phase;
    } else if (phase == 3) {
        g_assert_cmpint(a->exit_code, ==, 0);
        g_assert_nonnull(a->video_view.player);
        g_assert_false(a->scatter);
        libvlc_media_player_t *player = a->video_view.player;
        GPid decoder_pid = a->stages[1].pid;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->scatter_box), TRUE);
        g_assert_true(a->scatter);
        g_assert_true(gtk_widget_get_visible(a->scatter_view.frame));
        g_assert_true(a->video_view.player == player);
        g_assert_cmpint(a->stages[1].pid, ==, decoder_pid);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->usb_toggle),TRUE);
        g_assert_true(usb_receiver_running(a->usb_receiver));
        window_close(a->window, NULL, a);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv) {
    if (argc>1 && !strcmp(argv[1],"--decoder-worker=leansdr")) return leansdr_backend_main(argc-1,argv+1);
    if (argc > 1 && !strcmp(argv[1], "--decoder-worker=dvbt"))
        return dvbt_backend_main(argc-1, argv+1);
    g_assert_cmpint(argc, ==, 2);
    App a = {.sr = 125000, .tune_fd = -1};
    a.config = g_key_file_new();
    a.config_path = g_canonicalize_filename(argv[1], NULL);
    a.directory = g_path_get_dirname(a.config_path);
    g_assert_true(g_key_file_load_from_file(a.config, a.config_path, G_KEY_FILE_KEEP_COMMENTS, NULL));
    signal(SIGPIPE, SIG_IGN);
    g_timeout_add(150, exercise, &a);
    gui(&a);
    g_assert_false(a.running);
    g_assert_cmpint(a.exit_code, ==, 0);
    clear_stages(&a); g_key_file_unref(a.config); g_free(a.directory); g_free(a.config_path);
    return 0;
}
