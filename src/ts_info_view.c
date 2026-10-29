#include "ts_info_view.h"
#include <string.h>

static char *local_time(gint64 seconds, const char *format) {
    GDateTime *date=g_date_time_new_from_unix_local(seconds);
    char *text=date ? g_date_time_format(date,format) : g_strdup("Unknown time");
    if (date) g_date_time_unref(date);
    return text;
}
static char *received_text(gint64 stamp) {
    if (!stamp) return g_strdup("Waiting for information");
    char *time=local_time(stamp/G_USEC_PER_SEC,"%H:%M:%S");
    gboolean old=g_get_real_time()-stamp>120*G_USEC_PER_SEC;
    char *text=g_strdup_printf("Last received: %s%s",time,old ? " · no recent update" : "");
    g_free(time); return text;
}
static char *event_text(const TsInfoSnapshot *s) {
    GString *text=g_string_new(NULL);
    for (unsigned i=0;i<2;i++) {
        const TsInfoEvent *e=&s->events[i];
        g_string_append_printf(text,"%s\n",i ? "NEXT" : "NOW");
        if (!e->valid) { g_string_append(text,"No event information received\n\n"); continue; }
        g_string_append_printf(text,"%s\n",*e->title ? e->title : "Untitled event");
        if (e->start>=0) {
            char *start=local_time(e->start,"%d-%m-%Y %H:%M %Z");
            g_string_append(text,start); g_free(start);
            if (e->duration>=0) {
                char *end=local_time(e->start+e->duration,"%H:%M %Z");
                g_string_append_printf(text," – %s",end); g_free(end);
            }
        } else g_string_append(text,"Time not specified");
        if (e->duration>=0) g_string_append_printf(text," · %d min",e->duration/60);
        /* The language bytes are broadcast data, not necessarily ASCII. */
        if (g_ascii_isalpha(e->language[0]) && g_ascii_isalpha(e->language[1]) && g_ascii_isalpha(e->language[2]))
            g_string_append_printf(text," · %.3s",e->language);
        g_string_append_printf(text,"\n\n%s\n\n",e->text);
    }
    return g_string_free(text,FALSE);
}
static gboolean draw_page(GtkWidget *area, cairo_t *cr, gpointer data) {
    TsInfoView *v=data; TsInfoSnapshot *s=&v->snapshot;
    double width=gtk_widget_get_allocated_width(area),height=gtk_widget_get_allocated_height(area);
    cairo_set_source_rgb(cr,0,0,0); cairo_paint(cr);
    if (!s->page || s->width<=0 || s->height<=0) return FALSE;
    /* Teletext uses non-square pixels. Display the complete page as a classic
     * 4:3 screen; nearest-neighbour sampling preserves the character grid. */
    double w=MIN(width,height*4/3),h=w*3/4;
    cairo_surface_t *surface=cairo_image_surface_create_for_data(
        (unsigned char *)g_bytes_get_data(s->page,NULL),CAIRO_FORMAT_RGB24,s->width,s->height,s->width*4);
    cairo_save(cr); cairo_translate(cr,(width-w)/2,(height-h)/2);
    cairo_scale(cr,w/s->width,h/s->height); cairo_set_source_surface(cr,surface,0,0);
    cairo_pattern_set_filter(cairo_get_source(cr),CAIRO_FILTER_NEAREST);
    cairo_paint(cr); cairo_restore(cr); cairo_surface_destroy(surface);
    return FALSE;
}
static void update(TsInfoView *v, TsInfoSnapshot *s, gboolean force) {
    gboolean catalog=force || s->count!=v->snapshot.count || s->selected!=v->snapshot.selected ||
        memcmp(s->services,v->snapshot.services,sizeof(s->services));
    if (catalog) {
        v->updating=TRUE;
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(v->services));
        for (unsigned i=0;i<s->count;i++) {
            char id[12]; g_snprintf(id,sizeof(id),"%u",s->services[i].id);
            char *name=g_strdup_printf("%s · service %u",*s->services[i].name ? s->services[i].name : "Unnamed",s->services[i].id);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(v->services),id,name); g_free(name);
            if (s->services[i].id==s->selected) gtk_combo_box_set_active(GTK_COMBO_BOX(v->services),i);
        }
        gtk_widget_set_sensitive(v->services,s->count>1); v->updating=FALSE;
    }
    if (force || memcmp(s->events,v->snapshot.events,sizeof(s->events))) {
        char *text=event_text(s);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(v->eit)),text,-1); g_free(text);
    }
    gboolean ready[2]={s->events[0].valid || s->events[1].valid,s->page!=NULL};
    for (unsigned i=0;i<2;i++) {
        char *status;
        if (i==1 && !s->teletext_decoder && v->backend)
            status=g_strdup("Teletext decoder unavailable (FFmpeg needs libzvbi)");
        else if (i==1 && !ready[i])
            status=g_strdup(s->teletext_announced ? "Waiting for Teletext page 100" :
                s->pmt_seen ? "No Teletext announced for this service" : "Waiting for service information");
        else status=received_text(i ? s->teletext_received : s->eit_received);
        if (!i && s->eit_received && !ready[i]) { g_free(status); status=g_strdup("EIT received without current/next events"); }
        GtkStyleContext *style=gtk_widget_get_style_context(v->buttons[i]);
        if (ready[i]) gtk_style_context_add_class(style,"information-ready");
        else gtk_style_context_remove_class(style,"information-ready");
        char *tip=g_strdup_printf("%s%s\n%s",i ? "Teletext page 100" : "EIT",ready[i] ? " available" : "",status);
        gtk_widget_set_tooltip_text(v->buttons[i],tip); g_free(tip);
        gtk_label_set_text(GTK_LABEL(v->status[i]),status); g_free(status);
        atk_object_set_description(gtk_widget_get_accessible(v->buttons[i]),ready[i] ? "Information available" : "Waiting for information");
    }
    gboolean image_changed=force || s->page!=v->snapshot.page;
    ts_info_snapshot_clear(&v->snapshot); v->snapshot=*s; memset(s,0,sizeof(*s));
    if (image_changed) gtk_widget_queue_draw(v->page);
}
static void service_changed(GtkComboBox *box, gpointer data) {
    TsInfoView *v=data;
    if (v->updating) return;
    const char *id=gtk_combo_box_get_active_id(box);
    if (id && v->backend) {
        ts_info_backend_select(v->backend,(guint16)g_ascii_strtoull(id,NULL,10));
        v->next_tick=0; ts_info_view_tick(v,v->backend);
    }
}
static void hide_dialog(GtkDialog *dialog, gint response, gpointer data) {
    (void)response; (void)data; gtk_widget_hide(GTK_WIDGET(dialog));
}
static void open_information(GtkButton *button, gpointer data) {
    TsInfoView *v=data;
    v->next_tick=0; ts_info_view_tick(v,v->backend);
    gtk_widget_show_all(v->dialog);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(v->tabs),GTK_WIDGET(button)==v->buttons[1] ? 1 : 0);
    gtk_window_present(GTK_WINDOW(v->dialog));
}
void ts_info_view_create(TsInfoView *v, GtkWindow *parent, GtkWidget *bar) {
    v->dialog=gtk_dialog_new_with_buttons("EIT / Teletext",parent,GTK_DIALOG_DESTROY_WITH_PARENT,
        "Close",GTK_RESPONSE_CLOSE,NULL);
    gtk_widget_set_name(v->dialog,"ts-information");
    gtk_window_set_default_size(GTK_WINDOW(v->dialog),680,610);
    g_signal_connect(v->dialog,"response",G_CALLBACK(hide_dialog),v);
    g_signal_connect(v->dialog,"delete-event",G_CALLBACK(gtk_widget_hide_on_delete),NULL);
    GtkWidget *content=gtk_dialog_get_content_area(GTK_DIALOG(v->dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content),8); gtk_box_set_spacing(GTK_BOX(content),8);
    GtkWidget *row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    gtk_box_pack_start(GTK_BOX(row),gtk_label_new("Information for"),FALSE,FALSE,0);
    v->services=gtk_combo_box_text_new(); gtk_box_pack_start(GTK_BOX(row),v->services,TRUE,TRUE,0);
    gtk_widget_set_tooltip_text(v->services,"Select the service whose information is shown; this does not change the player.");
    gtk_box_pack_start(GTK_BOX(content),row,FALSE,FALSE,0);
    g_signal_connect(v->services,"changed",G_CALLBACK(service_changed),v);
    v->tabs=gtk_notebook_new(); gtk_box_pack_start(GTK_BOX(content),v->tabs,TRUE,TRUE,0);
    for (unsigned i=0;i<2;i++) {
        v->buttons[i]=gtk_button_new_with_label(i ? "Teletext" : "EIT");
        gtk_style_context_add_class(gtk_widget_get_style_context(v->buttons[i]),"information-button");
        gtk_box_pack_start(GTK_BOX(bar),v->buttons[i],FALSE,FALSE,0);
        g_signal_connect(v->buttons[i],"clicked",G_CALLBACK(open_information),v);
        GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);
        gtk_container_set_border_width(GTK_CONTAINER(box),8);
        v->status[i]=gtk_label_new(""); gtk_label_set_xalign(GTK_LABEL(v->status[i]),0);
        gtk_label_set_line_wrap(GTK_LABEL(v->status[i]),TRUE);
        gtk_box_pack_start(GTK_BOX(box),v->status[i],FALSE,FALSE,0);
        if (!i) {
            GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL);
            gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
            v->eit=gtk_text_view_new(); gtk_text_view_set_editable(GTK_TEXT_VIEW(v->eit),FALSE);
            gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(v->eit),FALSE);
            gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(v->eit),GTK_WRAP_WORD_CHAR);
            gtk_text_view_set_left_margin(GTK_TEXT_VIEW(v->eit),8); gtk_text_view_set_right_margin(GTK_TEXT_VIEW(v->eit),8);
            gtk_container_add(GTK_CONTAINER(scroll),v->eit); gtk_box_pack_start(GTK_BOX(box),scroll,TRUE,TRUE,0);
        } else {
            v->page=gtk_drawing_area_new(); gtk_widget_set_size_request(v->page,320,240);
            gtk_box_pack_start(GTK_BOX(box),v->page,TRUE,TRUE,0);
            g_signal_connect(v->page,"draw",G_CALLBACK(draw_page),v);
            atk_object_set_name(gtk_widget_get_accessible(v->page),"Teletext page 100");
        }
        gtk_notebook_append_page(GTK_NOTEBOOK(v->tabs),box,gtk_label_new(i ? "Teletext · 100" : "EIT"));
    }
    ts_info_view_reset(v);
}
void ts_info_view_tick(TsInfoView *v, TsInfoBackend *backend) {
    if (!v->dialog) return;
    gint64 now=g_get_monotonic_time();
    if (backend==v->backend && now<v->next_tick) return;
    v->backend=backend; v->next_tick=now+250000;
    TsInfoSnapshot s={0}; ts_info_backend_snapshot(backend,&s); update(v,&s,FALSE);
}
void ts_info_view_reset(TsInfoView *v) {
    v->backend=NULL; v->next_tick=0;
    if (!v->dialog) return;
    TsInfoSnapshot s={0}; update(v,&s,TRUE);
}
void ts_info_view_destroy(TsInfoView *v) {
    ts_info_snapshot_clear(&v->snapshot); v->backend=NULL;
    if (v->dialog) gtk_widget_destroy(v->dialog);
    v->dialog=NULL;
}
