/* Exercise actual GTK controls and pipeline callbacks on a headless display. */
#define main application_main
#include "../src/linrad_dvb.c"
#undef main

static void assert_no_tabs(GtkWidget *widget) {
    g_assert_false(GTK_IS_NOTEBOOK(widget));
    if (!GTK_IS_CONTAINER(widget)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (GList *p=children; p; p=p->next) assert_no_tabs(p->data);
    g_list_free(children);
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
    g_assert_cmpint(sendto(sender,&status,sizeof(status),0,(struct sockaddr *)&dest,sizeof(dest)),==,sizeof(status));
    g_assert_cmpint(sendto(sender,&queue,sizeof(queue),0,(struct sockaddr *)&dest,sizeof(dest)),==,sizeof(queue));
    g_assert_cmpint(sendto(sender,&points,sizeof(points),0,(struct sockaddr *)&dest,sizeof(dest)),==,sizeof(points));
    decoder_view_tick(v,TRUE,"DVB-T");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->service)),==,"Test <service>");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->provider)),==,"PE1ITR");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->buffer)),==,"25");
    g_assert_nonnull(strstr(gtk_label_get_label(GTK_LABEL(v->lamps[1])),"#36d866"));
    g_assert_cmpint(v->points.count,==,4);
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
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(v->service)),==,"—");
    decoder_view_stop(v); close(sender);
    decoder_view_tick(v,FALSE,"DVB-S2");
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(v->state)),"weergave volgt"));
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
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->power_label)), ==, "Start bestand");
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

static gboolean exercise(gpointer data) {
    App *a = data;
    static int phase, ticks;
    g_assert_cmpint(++ticks, <, 100);
    if (phase == 0) {
        exercise_decoder_view(a);
        exercise_file_selection(a);
        assert_no_tabs(a->window);
        g_assert_true(gtk_widget_get_parent(a->frequency_display) == gtk_widget_get_parent(a->power_button));
        g_assert_true(gtk_widget_get_parent(a->gain_display) == gtk_widget_get_parent(a->power_button));
        g_assert_false(gtk_widget_is_ancestor(a->standard_box, a->window));
        g_assert_false(gtk_widget_is_ancestor(a->renderer_box, a->window));
        g_assert_true(gtk_widget_is_ancestor(a->mode_summary, a->window));
        g_assert_true(gtk_widget_is_ancestor(a->renderer_summary, a->window));
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
        show_config(NULL, a);
        g_assert_true(gtk_widget_get_visible(a->config_dialog));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->center_box), 437.125);
        config_response(GTK_DIALOG(a->config_dialog), GTK_RESPONSE_CLOSE, a);
        g_assert_false(gtk_widget_get_visible(a->config_dialog));
        g_assert_cmpfloat(g_key_file_get_double(a->config, "linrad", "frequency", NULL), ==, 437125000);
        monitor_tick(a);
        g_assert_cmpfloat(a->iq_view.center, ==, 437125000);
        g_assert_cmpfloat(a->iq_view.rate, ==, 1010526);
        show_mode(NULL, a);
        gtk_dialog_response(GTK_DIALOG(a->mode_dialog), GTK_RESPONSE_CLOSE);
        g_assert_false(gtk_widget_get_visible(a->mode_dialog));
        show_renderer(NULL, a);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->renderer_box), RENDER_FFPLAY);
        g_assert_true(apply_renderer(a));
        g_assert_false(gtk_widget_get_sensitive(a->renderer_program));
        g_assert_true(gtk_widget_get_sensitive(a->scatter_box));
        g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(a->renderer_summary)), "ffplay"));
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
        g_assert_false(gtk_widget_get_sensitive(a->scatter_box));
        g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(a->renderer_summary)), "recording with spaces.ts"));
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
        g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(a->mode_summary)), "Bandbreedte: 40000 Hz"));
        pipeline = describe(a);
        g_assert_nonnull(strstr(pipeline, "'--sr' '40000'"));
        g_assert_nonnull(strstr(pipeline, "'--sample-rate' '196078'")); g_free(pipeline);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->renderer_box), RENDER_FILE);
        event.keyval = GDK_KEY_W;
        g_signal_emit_by_name(a->window, "key-press-event", &event, &handled);
        g_assert_true(handled);
        g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(a->status)), "Instellingen opgeslagen"));
        App restored = {.tune_fd = -1};
        restored.config = g_key_file_new();
        g_assert_true(g_key_file_load_from_file(restored.config, a->config_path, G_KEY_FILE_NONE, NULL));
        load_session(&restored);
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
        g_key_file_unref(restored.config);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->renderer_box), RENDER_CVLC);
        char *original_path = a->config_path;
        a->config_path = g_build_filename(original_path, "cannot-write.ini", NULL);
        save_settings(a);
        g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(a->status)), "Opslaan mislukt"));
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
        gtk_button_clicked(GTK_BUTTON(a->power_button));
        g_assert_true(a->running);
        g_assert_false(gtk_widget_get_sensitive(a->controls));
        g_assert_false(gtk_widget_get_sensitive(a->hardware));
        g_assert_false(gtk_widget_get_sensitive(a->renderer_panel));
        g_assert_true(gtk_widget_is_sensitive(a->filter_box));
        g_assert_true(gtk_widget_get_sensitive(a->power_button));
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->power_label)), ==, "Stop SDR · actief");
        g_assert_true(gtk_style_context_has_class(gtk_widget_get_style_context(a->power_button), "receiver-running"));
        ++phase;
    } else if (phase == 1) {
        g_assert_cmpint(a->exit_code, ==, 0);
        gtk_button_clicked(GTK_BUTTON(a->power_button));
        g_assert_false(gtk_widget_get_sensitive(a->power_button));
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->power_label)), ==, "SDR stopt…");
        ++phase;
    } else if (phase == 2 && !a->running) {
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->power_label)), ==, "Start SDR");
        g_assert_true(gtk_style_context_has_class(gtk_widget_get_style_context(a->power_button), "receiver-off"));
        g_assert_true(gtk_widget_get_sensitive(a->controls));
        g_assert_true(gtk_widget_get_sensitive(a->power_button));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->scatter_box), FALSE);
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->standard_box), 1);
        g_assert_cmpint(a->sr, ==, 250000);
        g_assert_false(gtk_widget_get_sensitive(a->gi_box));
        gtk_button_clicked(GTK_BUTTON(a->power_button));
        g_assert_true(a->running); ++phase;
    } else if (phase == 3) {
        g_assert_cmpint(a->exit_code, ==, 0);
        window_close(a->window, NULL, a);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv) {
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
