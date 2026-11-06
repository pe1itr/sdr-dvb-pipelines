/* DAB controls, snapshot attribution and station selection; no radio hardware. */
#define main application_main
#include "../src/linrad_dvb.c"
#undef main
#include "../components/dab/country.h"

static GtkWidget *tii_cell_at(App *a,int column,int row) {
    GtkWidget *cell=gtk_grid_get_child_at(GTK_GRID(a->dab.tii_rows),column,row);
    g_assert_nonnull(cell);return cell;
}

static gboolean exercise_dab_lifecycle(gpointer data) {
    App *a=data;
    static int phase,ticks;static GPid old_decoder,old_source;
    g_assert_cmpint(++ticks,<,150);
    if (!phase) {
        g_assert_true(a->running);g_assert_false(a->stopping);
        old_decoder=a->stages[1].pid;old_source=a->stages[0].pid;
        g_assert_cmpint(old_decoder,>,0);
        tune_step(a,-1);g_assert_false(a->stopping);g_assert_cmpuint(a->dab.generation,==,1);++phase;
    } else if (phase==1 && a->running && !a->stopping && a->dab.snapshot && dab_view_number(&a->dab,"generation")==1 && !dab_view_number(&a->dab,"tuning")) {
        g_assert_cmpint(a->stages[1].pid,==,old_decoder);
        g_assert_cmpint(a->stages[0].pid,==,old_source);g_assert_null(a->scatter_backend);
        g_assert_cmpint(a->stage_count,==,2);
        // Coalesce rapid A -> B -> A changes without restarting either process.
        tune_step(a,1);tune_step(a,-1);g_assert_cmpuint(a->dab.generation,==,3);++phase;
    } else if (phase==2 && a->dab.snapshot && dab_view_number(&a->dab,"generation")==3 && !dab_view_number(&a->dab,"tuning")) {
        g_assert_cmpint(a->stages[1].pid,==,old_decoder);g_assert_cmpint(a->stages[0].pid,==,old_source);
        power_clicked(NULL,a);++phase;
    } else if (phase==3 && !a->running) {
        g_assert_false(a->stopping);
        g_assert_cmpint(a->exit_code,==,0);gtk_main_quit();return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean exercise_dab(gpointer data) {
    App *a=data;
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->source_box),2);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->standard_box),4);
    g_assert_cmpint(a->sr,==,1536000);
    g_assert_null(receiver_offset_problem(a));
    g_assert_true(gtk_widget_get_visible(a->dab.info));
    g_assert_true(gtk_widget_get_visible(gtk_widget_get_parent(a->decoder_view.area)));
    g_assert_true(gtk_widget_get_visible(a->decoder_view.service_frame));
    g_assert_true(gtk_widget_get_parent(a->dab.tii_button)==a->iq_view.filter_controls);
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(a->dab.tii_button)),==,"TII");
    g_assert_false(gtk_style_context_has_class(gtk_widget_get_style_context(a->dab.tii_button),"information-ready"));
    g_assert_false(gtk_widget_get_visible(a->scatter_box));
    g_assert_false(gtk_widget_get_visible(a->information.buttons[0]));
    g_assert_false(gtk_widget_get_visible(a->information.buttons[1]));
    g_assert_false(gtk_widget_get_visible(a->programs.frame));
    g_assert_false(gtk_widget_get_visible(a->video_frame));
    g_assert_cmpint(a->stage_count,==,2);
    g_assert_nonnull(strstr(a->stages[1].args->pdata[0],"dab_worker"));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(a->frequency_display)),"12C · 227.360000"));
    gtk_button_clicked(GTK_BUTTON(a->dab_next));
    g_assert_cmpstr(dab_channels[dab_channel(a)].name,==,"12D");
    gtk_button_clicked(GTK_BUTTON(a->dab_previous));
    g_assert_cmpstr(dab_channels[dab_channel(a)].name,==,"12C");
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->dab_channel_box),0);
    gtk_button_clicked(GTK_BUTTON(a->dab_previous));
    g_assert_cmpint(dab_channel(a),==,0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->dab_channel_box),DAB_CHANNEL_COUNT-1);
    gtk_button_clicked(GTK_BUTTON(a->dab_next));
    g_assert_cmpint(dab_channel(a),==,DAB_CHANNEL_COUNT-1);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->dab_mixer_box),TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->dab_lo_box),TRUE);
    char *pipeline=describe(a);
    g_assert_nonnull(strstr(pipeline,"'--dab-if' '53000000'"));
    g_assert_nonnull(strstr(pipeline,"'--dab-lo-output'"));g_free(pipeline);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->dab_input_box),0);
    g_assert_nonnull(receiver_offset_problem(a));
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->dab_input_box),1);
    g_assert_null(receiver_offset_problem(a));
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->dab_profile_box),2);
    pipeline=describe(a);g_assert_nonnull(strstr(pipeline,"'--profile' 'aircraft'"));g_free(pipeline);

    char *state=g_build_filename(a->dab.directory,"state.json",NULL);
    const char *json="{\"channel\":\"9C\",\"frequency\":206352000,\"ensemble\":\"Test ensemble\","
        "\"synced\":1,\"signal_synced\":1,\"audio_active\":1,\"station\":\"Station one\",\"snr\":15.5,\"cfo\":120,\"mer\":12.5,"
        "\"scope_sequence\":1,\"scope_age_ms\":20,\"constellation\":[0.7,0.7,-0.7,-0.7],"
        "\"radio_ensemble\":\"Test ensemble\",\"ueid\":14909596,\"observed\":1793989196,\"radio_available\":1,"
        "\"services\":[{\"id\":4660,\"label\":\"Station one\",\"bitrate\":96},{\"id\":4661,\"label\":\"Station two\",\"bitrate\":64}],"
        "\"tii\":[{\"main\":19,\"sub\":4,\"confidence\":0.9,\"hits\":12,\"confirmed\":1,\"ambiguous\":0},"
        "{\"main\":46,\"sub\":15,\"confidence\":0.5,\"hits\":6,\"confirmed\":1,\"ambiguous\":1}]}";
    g_assert_true(g_file_set_contents(state,json,-1,NULL));g_free(state);
    dab_view_tick(&a->dab,TRUE);
    g_assert_true(gtk_style_context_has_class(gtk_widget_get_style_context(a->dab.tii_button),"information-ready"));
    g_assert_true(gtk_widget_get_visible(a->decoder_view.country));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->decoder_view.country)),==,"NL");
    g_assert_cmpstr(dab_country_code(0xE08001),==,"PS");
    g_assert_cmpstr(dab_country_code(0xE0D001),==,"DE");
    g_assert_cmpstr(dab_country_code(0xE01001),==,"DE");
    g_assert_cmpstr(dab_country_code(0xE1C001),==,"GB");
    g_assert_cmpstr(dab_country_code(0xF03001),==,"AU");
    g_assert_cmpstr(dab_country_code(0x008001),==,"");
    g_assert_cmpstr(dab_country_code(0xE30001),==,"");
    g_assert_cmpstr(dab_country_code(0xE38001),==,"NL");
    g_assert_cmpstr(dab_country_code(0xE48001),==,"PT");
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(a->dab.label)),"Test ensemble"));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->decoder_view.provider)),==,"Station one");
    g_assert_cmpuint(a->decoder_view.status.locks,==,15);
    g_assert_cmpfloat(a->decoder_view.status.snr_db,==,15.5);
    g_assert_cmpuint(a->decoder_view.points.count,==,2);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(g_object_get_data(G_OBJECT(a->decoder_view.service),"caption"))),==,"Ensemble");
    const char *screenshot=g_getenv("LINRAD_DAB_SCREENSHOT");
    if (screenshot) {
        GtkRequisition minimum,natural;gtk_widget_get_preferred_size(a->window,&minimum,&natural);
        GtkAllocation allocation={0,0,1600,900};gtk_widget_size_allocate(a->window,&allocation);
        cairo_surface_t *surface=cairo_image_surface_create(CAIRO_FORMAT_RGB24,
            gtk_widget_get_allocated_width(a->window),gtk_widget_get_allocated_height(a->window));
        cairo_t *cr=cairo_create(surface);gtk_widget_draw(a->window,cr);
        g_assert_cmpint(cairo_surface_write_to_png(surface,screenshot),==,CAIRO_STATUS_SUCCESS);
        cairo_destroy(cr);cairo_surface_destroy(surface);
    }
    GList *stations=gtk_container_get_children(GTK_CONTAINER(a->dab.stations));
    g_assert_cmpuint(g_list_length(stations),==,2);
    gtk_button_clicked(GTK_BUTTON(stations->data));g_list_free(stations);
    char *command=g_build_filename(a->dab.directory,"station",NULL), *selected=NULL;
    g_assert_true(g_file_get_contents(command,&selected,NULL,NULL));g_assert_cmpstr(selected,==,"0x1234\n");
    g_free(command);g_free(selected);
    gtk_button_clicked(GTK_BUTTON(a->dab.tii_button));
    g_assert_true(gtk_widget_get_visible(a->dab.tii_dialog));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(tii_cell_at(a,0,1))),==,"1904");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(tii_cell_at(a,1,1))),==,"90%");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(tii_cell_at(a,2,1))),==,"12");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(tii_cell_at(a,3,1))),==,"confirmed");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(tii_cell_at(a,4,1))),==,"New");
    GtkWidget *log=tii_cell_at(a,5,1);
    g_assert_true(gtk_widget_get_sensitive(log));gtk_button_clicked(GTK_BUTTON(log));
    g_assert_nonnull(a->reception_dialog);
    gtk_dialog_response(GTK_DIALOG(a->reception_dialog),GTK_RESPONSE_CANCEL);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(tii_cell_at(a,4,1))),==,"New");
    g_assert_false(gtk_widget_get_sensitive(tii_cell_at(a,5,2)));
    // A saved code on another ensemble must not hide this new transmitter.
    char *log_path=reception_log_path(a);
    g_assert_true(reception_log_append(log_path,"2026-11-06T12:00:00Z",206352000,
        "Other ensemble","","DAB","1904","file","","1536000","auto","","",NULL));
    dab_view_refresh_tii(&a->dab);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(tii_cell_at(a,4,1))),==,"New");
    gtk_button_clicked(GTK_BUTTON(tii_cell_at(a,5,1)));
    gtk_dialog_response(GTK_DIALOG(a->reception_dialog),GTK_RESPONSE_ACCEPT);
    g_assert_null(a->reception_dialog);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(tii_cell_at(a,4,1))),==,"✓ Logged");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(tii_cell_at(a,4,2))),==,"New");
    // Reopening reads saved history, including a pre-existing spaced TII code.
    g_assert_true(reception_log_append(log_path,"2026-11-06T12:00:00Z",206352000,
        "Test ensemble","","DAB","46 15","file","","1536000","auto","","",NULL));
    gtk_dialog_response(GTK_DIALOG(a->dab.tii_dialog),GTK_RESPONSE_CLOSE);
    gtk_button_clicked(GTK_BUTTON(a->dab.tii_button));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(tii_cell_at(a,4,2))),==,"✓ Logged");
    g_assert_false(gtk_widget_get_sensitive(tii_cell_at(a,5,2)));
    g_free(log_path);
    // Empty captures reset the indicator; an unknown ECC never guesses a country.
    state=g_build_filename(a->dab.directory,"state.json",NULL);
    g_assert_true(g_file_set_contents(state,"{\"ueid\":32769,\"tii\":[]}",-1,NULL));
    dab_view_tick(&a->dab,TRUE);
    g_assert_false(gtk_style_context_has_class(gtk_widget_get_style_context(a->dab.tii_button),"information-ready"));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->decoder_view.country)),==,"—");
    g_assert_true(g_file_set_contents(state,json,-1,NULL));g_free(state);
    dab_view_tick(&a->dab,TRUE);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(tii_cell_at(a,4,1))),==,"✓ Logged");
    dab_view_mute(&a->dab,TRUE);
    command=g_build_filename(a->dab.directory,"mute",NULL);
    g_assert_true(g_file_get_contents(command,&selected,NULL,NULL));g_assert_cmpstr(selected,==,"1");
    g_free(command);g_free(selected);
    dab_view_reset(&a->dab);
    g_assert_null(a->dab.snapshot);
    g_assert_false(gtk_style_context_has_class(gtk_widget_get_style_context(a->dab.tii_button),"information-ready"));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(a->decoder_view.country)),==,"—");
    g_assert_cmpint(gtk_image_get_storage_type(GTK_IMAGE(a->dab.image)),==,GTK_IMAGE_EMPTY);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->standard_box),0);
    g_assert_true(gtk_widget_get_visible(a->decoder_view.service_frame));
    g_assert_false(gtk_widget_get_visible(a->dab.tii_button));
    g_assert_true(gtk_widget_get_visible(a->scatter_box));
    g_assert_true(gtk_widget_get_visible(a->video_frame));
    g_assert_false(gtk_widget_get_visible(a->dab.tii_dialog));
    decoder_view_tick(&a->decoder_view,FALSE,"DVB-S");
    g_assert_false(gtk_widget_get_visible(a->decoder_view.country));
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->standard_box),4);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->dab_lo_box),FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->dab_mixer_box),FALSE);
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->dab_profile_box),0);
    // The Python harness replaces [paths] sdr with an IQ generator before launch.
    char *source=setting(a,"paths","sdr");g_assert_true(g_str_has_suffix(source,"fake-dab-source"));g_free(source);
    power_clicked(NULL,a);
    g_timeout_add(50,exercise_dab_lifecycle,a);return G_SOURCE_REMOVE;
}
int main(int argc,char **argv) {
    g_assert_cmpint(argc,==,2);
    App a={.sr=125000,.tune_fd=-1};
    a.config=g_key_file_new();a.config_path=g_canonicalize_filename(argv[1],NULL);
    a.directory=g_path_get_dirname(a.config_path);
    g_assert_true(g_key_file_load_from_file(a.config,a.config_path,0,NULL));
    g_timeout_add(200,exercise_dab,&a);gui(&a);
    clear_stages(&a);g_key_file_unref(a.config);g_free(a.config_path);g_free(a.directory);
    return 0;
}
