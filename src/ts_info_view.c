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
    /* The containing screen keeps the TV's 4:3 proportions, including the
     * header and information bar. Preserve every broadcast row and use
     * nearest-neighbour sampling for the character grid. */
    cairo_surface_t *surface=cairo_image_surface_create_for_data(
        (unsigned char *)g_bytes_get_data(s->page,NULL),CAIRO_FORMAT_RGB24,s->width,s->height,s->width*4);
    cairo_save(cr);
    cairo_scale(cr,width/s->width,height/s->height); cairo_set_source_surface(cr,surface,0,0);
    cairo_pattern_set_filter(cairo_get_source(cr),CAIRO_FILTER_NEAREST);
    cairo_paint(cr); cairo_restore(cr); cairo_surface_destroy(surface);
    return FALSE;
}
static void update(TsInfoView *v, TsInfoSnapshot *s, gboolean force) {
    gboolean catalog=force || s->count!=v->snapshot.count || s->selected!=v->snapshot.selected ||
        memcmp(s->services,v->snapshot.services,sizeof(s->services));
    if (catalog) {
        v->updating=TRUE;
        for (unsigned window=0;window<2;window++) {
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(v->services[window]));
            for (unsigned i=0;i<s->count;i++) {
                char id[12]; g_snprintf(id,sizeof(id),"%u",s->services[i].id);
                char *name=g_strdup_printf("%s · service %u",*s->services[i].name ? s->services[i].name : "Unnamed",s->services[i].id);
                gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(v->services[window]),id,name); g_free(name);
                if (s->services[i].id==s->selected) gtk_combo_box_set_active(GTK_COMBO_BOX(v->services[window]),i);
            }
            gtk_widget_set_sensitive(v->services[window],s->count>1);
        }
        v->updating=FALSE;
        const char *station="TELETEXT";
        for (unsigned i=0;i<s->count;i++)
            if (s->services[i].id==s->selected && *s->services[i].name) station=s->services[i].name;
        gtk_label_set_text(GTK_LABEL(v->station),station);
    }
    char *clock=local_time(g_get_real_time()/G_USEC_PER_SEC,"%H:%M:%S");
    gtk_label_set_text(GTK_LABEL(v->clock),clock); g_free(clock);
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
    GtkWidget *dialog=v->dialogs[GTK_WIDGET(button)==v->buttons[1] ? 1 : 0];
    gtk_widget_show_all(dialog);
    gtk_window_present(GTK_WINDOW(dialog));
}
void ts_info_view_create(TsInfoView *v, GtkWindow *parent, GtkWidget *bar) {
    for (unsigned i=0;i<2;i++) {
        v->dialogs[i]=gtk_dialog_new_with_buttons(i ? "Teletext" : "EIT",parent,GTK_DIALOG_DESTROY_WITH_PARENT,
            "Close",GTK_RESPONSE_CLOSE,NULL);
        gtk_widget_set_name(v->dialogs[i],i ? "teletext-information" : "eit-information");
        gtk_window_set_default_size(GTK_WINDOW(v->dialogs[i]),680,610);
        g_signal_connect(v->dialogs[i],"response",G_CALLBACK(hide_dialog),v);
        g_signal_connect(v->dialogs[i],"delete-event",G_CALLBACK(gtk_widget_hide_on_delete),NULL);
        GtkWidget *content=gtk_dialog_get_content_area(GTK_DIALOG(v->dialogs[i]));
        gtk_container_set_border_width(GTK_CONTAINER(content),8); gtk_box_set_spacing(GTK_BOX(content),8);
        GtkWidget *row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
        gtk_box_pack_start(GTK_BOX(row),gtk_label_new("Information for"),FALSE,FALSE,0);
        v->services[i]=gtk_combo_box_text_new(); gtk_box_pack_start(GTK_BOX(row),v->services[i],TRUE,TRUE,0);
        gtk_widget_set_tooltip_text(v->services[i],"Select the service for both information windows; this does not change the player.");
        gtk_box_pack_start(GTK_BOX(content),row,FALSE,FALSE,0);
        g_signal_connect(v->services[i],"changed",G_CALLBACK(service_changed),v);
        v->buttons[i]=gtk_button_new_with_label(i ? "Teletext" : "EIT");
        gtk_style_context_add_class(gtk_widget_get_style_context(v->buttons[i]),"information-button");
        gtk_box_pack_start(GTK_BOX(bar),v->buttons[i],FALSE,FALSE,0);
        g_signal_connect(v->buttons[i],"clicked",G_CALLBACK(open_information),v);
        GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);
        gtk_box_pack_start(GTK_BOX(content),box,TRUE,TRUE,0);
        v->status[i]=gtk_label_new(""); gtk_label_set_xalign(GTK_LABEL(v->status[i]),0);
        if (!i) {
            gtk_label_set_line_wrap(GTK_LABEL(v->status[i]),TRUE);
            gtk_box_pack_start(GTK_BOX(box),v->status[i],FALSE,FALSE,0);
            GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL);
            gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
            v->eit=gtk_text_view_new(); gtk_text_view_set_editable(GTK_TEXT_VIEW(v->eit),FALSE);
            gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(v->eit),FALSE);
            gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(v->eit),GTK_WRAP_WORD_CHAR);
            gtk_text_view_set_left_margin(GTK_TEXT_VIEW(v->eit),8); gtk_text_view_set_right_margin(GTK_TEXT_VIEW(v->eit),8);
            gtk_container_add(GTK_CONTAINER(scroll),v->eit); gtk_box_pack_start(GTK_BOX(box),scroll,TRUE,TRUE,0);
        } else {
            GtkWidget *aspect=gtk_aspect_frame_new(NULL,.5,.5,4.0/3.0,FALSE);
            gtk_frame_set_shadow_type(GTK_FRAME(aspect),GTK_SHADOW_NONE);
            gtk_box_pack_start(GTK_BOX(box),aspect,TRUE,TRUE,0);
            GtkWidget *screen=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
            gtk_widget_set_name(screen,"teletext-screen");
            gtk_container_add(GTK_CONTAINER(aspect),screen);
            GtkWidget *header=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,16);
            gtk_widget_set_name(header,"teletext-header");
            v->station=gtk_label_new("TELETEXT");
            gtk_label_set_xalign(GTK_LABEL(v->station),0);
            gtk_label_set_ellipsize(GTK_LABEL(v->station),PANGO_ELLIPSIZE_END);
            gtk_box_pack_start(GTK_BOX(header),v->station,TRUE,TRUE,0);
            v->clock=gtk_label_new("");
            gtk_widget_set_tooltip_text(v->clock,"Local time");
            gtk_box_pack_start(GTK_BOX(header),v->clock,FALSE,FALSE,0);
            v->page_number=gtk_label_new("P100");
            gtk_widget_set_name(v->page_number,"teletext-page-number");
            atk_object_set_name(gtk_widget_get_accessible(v->page_number),"Page 100");
            gtk_box_pack_end(GTK_BOX(header),v->page_number,FALSE,FALSE,0);
            gtk_box_pack_start(GTK_BOX(screen),header,FALSE,FALSE,0);
            gtk_widget_set_name(v->status[i],"teletext-status");
            gtk_label_set_ellipsize(GTK_LABEL(v->status[i]),PANGO_ELLIPSIZE_END);
            gtk_box_pack_start(GTK_BOX(screen),v->status[i],FALSE,FALSE,0);
            v->page=gtk_drawing_area_new(); gtk_widget_set_size_request(v->page,320,240);
            gtk_box_pack_start(GTK_BOX(screen),v->page,TRUE,TRUE,0);
            g_signal_connect(v->page,"draw",G_CALLBACK(draw_page),v);
            atk_object_set_name(gtk_widget_get_accessible(v->page),"Teletext page 100");
        }
    }
    ts_info_view_reset(v);
}
void ts_info_view_tick(TsInfoView *v, TsInfoBackend *backend) {
    if (!v->dialogs[0]) return;
    gint64 now=g_get_monotonic_time();
    if (backend==v->backend && now<v->next_tick) return;
    v->backend=backend; v->next_tick=now+250000;
    TsInfoSnapshot s={0}; ts_info_backend_snapshot(backend,&s); update(v,&s,FALSE);
}
void ts_info_view_reset(TsInfoView *v) {
    v->backend=NULL; v->next_tick=0;
    if (!v->dialogs[0]) return;
    TsInfoSnapshot s={0}; update(v,&s,TRUE);
}
void ts_info_view_destroy(TsInfoView *v) {
    ts_info_snapshot_clear(&v->snapshot); v->backend=NULL;
    for (unsigned i=0;i<2;i++) {
        if (v->dialogs[i]) gtk_widget_destroy(v->dialogs[i]);
        v->dialogs[i]=NULL;
    }
}
