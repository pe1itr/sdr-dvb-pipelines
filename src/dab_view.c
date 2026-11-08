#include "dab_view.h"
#include <glib-unix.h>
#include "../components/dab/country.h"
#include <glib/gstdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>

static const char *text(json_object *o,const char *key) {
    json_object *v=NULL;
    return json_object_object_get_ex(o,key,&v) && v ? json_object_get_string(v) : "";
}
static double number(json_object *o,const char *key) {
    json_object *v=NULL; json_object_object_get_ex(o,key,&v); return json_object_get_double(v);
}
const char *dab_view_text(DabView *v,const char *key) {return text(v->snapshot,key);}
double dab_view_number(DabView *v,const char *key) {return number(v->snapshot,key);}
static void clear(GtkWidget *box) {
    GList *children=gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *p=children;p;p=p->next) gtk_widget_destroy(p->data);
    g_list_free(children);
}
static void station_clicked(GtkButton *button,gpointer data) {
    DabView *v=data;
    if (!v->active || !v->directory) return;
    guint id=GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button),"service"));
    char command[24]; g_snprintf(command,sizeof(command),"0x%X\n",id);
    char *path=g_build_filename(v->directory,"station",NULL);
    GError *error=NULL;
    if (!g_file_set_contents(path,command,-1,&error)) {
        gtk_label_set_text(GTK_LABEL(v->caption),error->message);g_clear_error(&error);
    } else {
        gtk_image_clear(GTK_IMAGE(v->image));v->slide=0;
        gtk_label_set_text(GTK_LABEL(v->caption),"Tuning station…");
    }
    g_free(path);
}
static void tii_log(GtkButton *button,gpointer data) {
    DabView *v=data;
    const DabReception *entry=g_object_get_data(G_OBJECT(button),"reception");
    if (entry && v->log) v->log(entry,v->data);
}
static GtkWidget *tii_cell(DabView *v,int column,int row,const char *text,gboolean numeric) {
    GtkWidget *label=gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label),numeric ? 1 : 0);
    gtk_widget_set_hexpand(label,column==3);
    gtk_grid_attach(GTK_GRID(v->tii_rows),label,column,row,1,1);
    return label;
}
static void tii_update(DabView *v) {
    if (!v->tii_rows) return;
    json_object *items=NULL; json_object_object_get_ex(v->snapshot,"tii",&items);
    size_t n=items ? json_object_array_length(items) : 0;
    GtkStyleContext *style=gtk_widget_get_style_context(v->tii_button);
    if (n) gtk_style_context_add_class(style,"information-ready");
    else gtk_style_context_remove_class(style,"information-ready");
    char *signature=g_strdup_printf("%s|%s|%.0f|%.0f|%.0f|%d",items ? json_object_to_json_string(items) : "",
        dab_view_text(v,"ensemble"),dab_view_number(v,"observed"),dab_view_number(v,"ueid"),
        dab_view_number(v,"frequency"),v->active);
    if (!g_strcmp0(signature,v->tii_signature)) {g_free(signature);return;}
    g_free(v->tii_signature);v->tii_signature=signature;clear(v->tii_rows);
    const char *headings[]={"TII","Confidence","Hits","Status","Logbook",""};
    for (unsigned i=0;i<G_N_ELEMENTS(headings);++i) tii_cell(v,i,0,headings[i],i==1 || i==2);
    if (!n) gtk_grid_attach(GTK_GRID(v->tii_rows),gtk_label_new("No TII codes in the latest capture."),0,1,6,1);
    char ensemble[128];
    g_strlcpy(ensemble,dab_view_text(v,"ensemble"),sizeof(ensemble));
    if (!*ensemble) g_snprintf(ensemble,sizeof(ensemble),"UEId %06X",(unsigned)dab_view_number(v,"ueid"));
    GHashTable *logged=n && v->logged_tii ? v->logged_tii(ensemble,v->data) : NULL;
    for (size_t i=0;i<n;++i) {
        json_object *item=json_object_array_get_idx(items,i);
        gboolean confirmed=number(item,"confirmed") && !number(item,"ambiguous");
        GtkWidget *button=gtk_button_new_with_label("Log");
        gtk_widget_set_sensitive(button,confirmed && v->active && dab_view_number(v,"ueid")>0);
        gtk_widget_set_tooltip_text(button,"Log this confirmed TII with its ensemble, frequency and observation time");
        DabReception *entry=g_new0(DabReception,1);
        g_strlcpy(entry->ensemble,ensemble,sizeof(entry->ensemble));
        g_snprintf(entry->tii,sizeof(entry->tii),"%02d%02d",(int)number(item,"main"),(int)number(item,"sub"));
        entry->frequency=dab_view_number(v,"frequency");entry->observed=dab_view_number(v,"observed");
        char value[32];int row=(int)i+1;
        tii_cell(v,0,row,entry->tii,FALSE);
        g_snprintf(value,sizeof(value),"%.0f%%",number(item,"confidence")*100);tii_cell(v,1,row,value,TRUE);
        g_snprintf(value,sizeof(value),"%.0f",number(item,"hits"));tii_cell(v,2,row,value,TRUE);
        tii_cell(v,3,row,number(item,"ambiguous") ? "ambiguous" : confirmed ? "confirmed" : "candidate",FALSE);
        gboolean was_logged=dab_view_number(v,"ueid")>0 && logged && g_hash_table_contains(logged,entry->tii);
        GtkWidget *history=tii_cell(v,4,row,dab_view_number(v,"ueid")<=0 ? "—" : !logged ? "Unknown" : "New",FALSE);
        if (was_logged) gtk_label_set_markup(GTK_LABEL(history),"<span foreground=\"#36d866\">✓</span> Logged");
        g_object_set_data_full(G_OBJECT(button),"reception",entry,g_free);
        g_signal_connect(button,"clicked",G_CALLBACK(tii_log),v);
        gtk_grid_attach(GTK_GRID(v->tii_rows),button,5,row,1,1);
    }
    if (logged) g_hash_table_unref(logged);
    gtk_widget_show_all(v->tii_rows);
}
void dab_view_refresh_tii(DabView *v) {
    g_clear_pointer(&v->tii_signature,g_free);tii_update(v);
}
static void show_tii(GtkButton *button,gpointer data) {
    (void)button;DabView *v=data;dab_view_refresh_tii(v);gtk_widget_show_all(v->tii_dialog);
    gtk_window_present(GTK_WINDOW(v->tii_dialog));
}
static void hide_tii(GtkDialog *dialog,gint response,gpointer data) {
    (void)response;(void)data;gtk_widget_hide(GTK_WIDGET(dialog));
}
void dab_view_create(DabView *v,DecoderView *decoder,GtkWindow *parent,void (*log)(const DabReception*,gpointer),GHashTable *(*logged_tii)(const char*,gpointer),gpointer data) {
    v->parent=parent;v->log=log;v->logged_tii=logged_tii;v->data=data;
    v->decoder=decoder;v->info=decoder->service_frame;v->label=decoder->service;
    v->tii_button=gtk_button_new_with_label("TII");
    gtk_style_context_add_class(gtk_widget_get_style_context(v->tii_button),"information-button");
    gtk_widget_set_tooltip_text(v->tii_button,"Show received TII codes; green when the latest capture contains codes");
    GtkWidget *box;
    v->image_frame=gtk_frame_new("Station slideshow");
    box=gtk_box_new(GTK_ORIENTATION_VERTICAL,4);gtk_container_add(GTK_CONTAINER(v->image_frame),box);
    v->image=gtk_image_new();gtk_widget_set_size_request(v->image,320,240);
    v->caption=gtk_label_new("Waiting for stations…");gtk_label_set_line_wrap(GTK_LABEL(v->caption),TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(v->caption),42);
    gtk_box_pack_start(GTK_BOX(box),v->image,TRUE,TRUE,0);gtk_box_pack_end(GTK_BOX(box),v->caption,FALSE,FALSE,4);
    v->stations_frame=gtk_frame_new("Stations in ensemble");
    GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL);gtk_widget_set_size_request(scroll,220,280);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(v->stations_frame),scroll);
    v->stations=gtk_box_new(GTK_ORIENTATION_VERTICAL,3);gtk_container_add(GTK_CONTAINER(scroll),v->stations);
    v->tii_dialog=gtk_dialog_new_with_buttons("DAB TII codes",parent,GTK_DIALOG_DESTROY_WITH_PARENT,"Close",GTK_RESPONSE_CLOSE,NULL);
    gtk_window_set_default_size(GTK_WINDOW(v->tii_dialog),620,350);
    scroll=gtk_scrolled_window_new(NULL,NULL);gtk_widget_set_vexpand(scroll,TRUE);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(v->tii_dialog))),scroll,TRUE,TRUE,8);
    v->tii_rows=gtk_grid_new();gtk_container_add(GTK_CONTAINER(scroll),v->tii_rows);
    gtk_grid_set_column_spacing(GTK_GRID(v->tii_rows),18);
    gtk_grid_set_row_spacing(GTK_GRID(v->tii_rows),6);
    gtk_container_set_border_width(GTK_CONTAINER(v->tii_rows),12);
    g_signal_connect(v->tii_dialog,"delete-event",G_CALLBACK(gtk_widget_hide_on_delete),NULL);
    g_signal_connect(v->tii_dialog,"response",G_CALLBACK(hide_tii),v);
    g_signal_connect(v->tii_button,"clicked",G_CALLBACK(show_tii),v);
}
void dab_view_mute(DabView *v,gboolean muted) {
    if (!v->directory) return;
    char *path=g_build_filename(v->directory,"mute",NULL);
    g_file_set_contents(path,muted ? "1" : "0",-1,NULL);g_free(path);
}
/* A bounded newline-framed stream from the worker; retain only the latest snapshot. */
void dab_view_feed(DabView *v,const char *data,gsize length) {
    if (!v->status_line) v->status_line=g_string_new(NULL);
    for (gsize i=0;i<length;++i) {
        if (data[i]=='\n') {
            if (!v->status_discard && v->status_line->len) {
                g_free(v->pending_json);v->pending_json=g_strdup(v->status_line->str);
            }
            g_string_truncate(v->status_line,0);v->status_discard=FALSE;
        } else if (!v->status_discard) {
            if (!data[i] || v->status_line->len>=262144) {v->status_discard=TRUE;g_string_truncate(v->status_line,0);}
            else g_string_append_c(v->status_line,data[i]);
        }
    }
}
static gboolean read_status(gint fd,GIOCondition condition,gpointer data) {
    DabView *v=data;char buffer[16384];
    for (int i=0;i<16;++i) {
        ssize_t n=read(fd,buffer,sizeof(buffer));
        if (n>0) {dab_view_feed(v,buffer,n);continue;}
        if (n<0 && errno==EINTR) continue;
        if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK) && !(condition&G_IO_NVAL)) return G_SOURCE_CONTINUE;
        close(fd);v->status_watch=0;return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}
void dab_view_watch(DabView *v,int fd) {
    if (v->status_watch) {g_source_remove(v->status_watch);close(v->status_fd);}
    v->status_fd=fd;
    v->status_watch=g_unix_fd_add(fd,G_IO_IN|G_IO_HUP|G_IO_ERR,read_status,v);
}
static void clear_reception(DabView *v) {
    if (v->snapshot) json_object_put(v->snapshot);
    v->snapshot=NULL;v->active=FALSE;v->slide=0;v->snapshot_time=0;
    g_clear_pointer(&v->pending_json,g_free);
    g_clear_pointer(&v->last_json,g_free);g_clear_pointer(&v->station_signature,g_free);g_clear_pointer(&v->tii_signature,g_free);
    v->scope_sequence=0;
    if (v->decoder) {
        v->decoder->status_time=v->decoder->points_time=0;
        memset(v->decoder->history_time,0,sizeof(v->decoder->history_time));
        v->decoder->selected_video_codec[0]=v->decoder->selected_audio_codec[0]=0;
        gtk_label_set_text(GTK_LABEL(v->decoder->country),"—");
        gtk_widget_set_tooltip_text(v->decoder->country,NULL);
    }
    if (!v->info) return;
    gtk_style_context_remove_class(gtk_widget_get_style_context(v->tii_button),"information-ready");
    clear(v->stations);clear(v->tii_rows);gtk_image_clear(GTK_IMAGE(v->image));
    gtk_label_set_text(GTK_LABEL(v->label),"Waiting for ensemble");gtk_label_set_text(GTK_LABEL(v->caption),"Waiting for stations…");
}
void dab_view_reset(DabView *v) {
    if (v->status_watch) {g_source_remove(v->status_watch);close(v->status_fd);v->status_watch=0;}
    if (v->status_line) {g_string_free(v->status_line,TRUE);v->status_line=NULL;}
    v->status_discard=FALSE;
    clear_reception(v);v->generation=0;
    if (v->directory) {
        const char *names[]={"state.json","state.json.tmp","station","slide","slide.tmp","mute","channel","tuned","tuned.tmp"};
        for (unsigned i=0;i<G_N_ELEMENTS(names);++i) {char *p=g_build_filename(v->directory,names[i],NULL);g_unlink(p);g_free(p);}
    }
}
gboolean dab_view_retune(DabView *v,int fd,const char *channel,guint64 hz) {
    if (fd<0 || !v->directory) return FALSE;
    guint64 generation=v->generation+1;
    char request[80],command[80];
    g_snprintf(request,sizeof(request),"%" G_GUINT64_FORMAT " %s %" G_GUINT64_FORMAT "\n",generation,channel,hz);
    char *path=g_build_filename(v->directory,"channel",NULL);
    gboolean ok=g_file_set_contents(path,request,-1,NULL);g_free(path);
    if (!ok) return FALSE;
    int length=g_snprintf(command,sizeof(command),"dab %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT "\n",generation,hz);
    ssize_t sent;
    do {sent=write(fd,command,length);} while (sent<0 && errno==EINTR);
    // Even a failed source write invalidates old channel evidence; show waiting.
    clear_reception(v);v->generation=generation;
    return sent==length;
}
static double metric(json_object *root,const char *key) {
    json_object *value=NULL;
    return json_object_object_get_ex(root,key,&value) && value ? json_object_get_double(value) : NAN;
}
static void update_decoder(DabView *v,json_object *root,const char *ensemble) {
    DecoderView *d=v->decoder;if (!d) return;
    DecoderStatus *s=&d->status;memset(s,0,sizeof(*s));
    s->lock_valid=15;
    s->locks=(number(root,"signal_synced") ? DECODER_OFDM : 0) |
        (number(root,"synced") ? DECODER_FEC : 0) |
        (number(root,"audio_active") ? DECODER_TS : 0) |
        (*ensemble ? DECODER_SERVICE : 0);
    s->snr_db=metric(root,"snr");s->pilot_lock=metric(root,"cp");s->cfo_hz=metric(root,"cfo");
    s->rms_dbfs=metric(root,"rms");s->peak_dbfs=metric(root,"peak");s->mer_db=metric(root,"mer");
    s->buffer_valid=1;s->buffer_percent=number(root,"capture_percent");
    s->packets=number(root,"fib_ok");s->written_packets=number(root,"audio_frames");
    s->rs_ok=number(root,"figs");s->rs_uncorrectable=number(root,"fib_bad");s->cc_errors=number(root,"dropped_iq");
    g_strlcpy(s->service,ensemble,sizeof(s->service));g_strlcpy(s->provider,text(root,"station"),sizeof(s->provider));
    g_strlcpy(s->modulation,"DAB Mode I · DQPSK",sizeof(s->modulation));
    g_strlcpy(s->fec,"1/3 FIC",sizeof(s->fec));g_strlcpy(s->guard,"504/2048",sizeof(s->guard));
    g_strlcpy(d->selected_video_codec,number(root,"slide") ? "MOT" : "",sizeof(d->selected_video_codec));
    g_strlcpy(d->selected_audio_codec,*text(root,"station") ? "AAC" : "",sizeof(d->selected_audio_codec));
    d->status_time=g_get_monotonic_time();
    guint64 sequence=number(root,"scope_sequence");
    json_object *points=NULL;json_object_object_get_ex(root,"constellation",&points);
    if (sequence!=v->scope_sequence && points && number(root,"scope_age_ms")<1500) {
        v->scope_sequence=sequence;d->points.count=MIN(json_object_array_length(points)/2,DECODER_POINTS);
        for (unsigned i=0;i<2*d->points.count;++i) d->points.iq[i]=json_object_get_double(json_object_array_get_idx(points,i));
        d->points_time=d->status_time;unsigned slot=d->history_next++%DECODER_HISTORY_FRAMES;
        d->history[slot]=d->points;d->history_time[slot]=d->points_time;
    }
}
static void read_state(DabView *v,gboolean active) {
    v->active=active;
    if (v->info) {
        gtk_widget_set_sensitive(v->stations,active);
        gtk_widget_set_sensitive(v->tii_rows,active);
    }
    if (!active || !v->pending_json) return;
    char *contents=g_steal_pointer(&v->pending_json), *path=NULL;
    json_object *root=json_tokener_parse(contents);
    if (!root || !json_object_is_type(root,json_type_object)) {json_object_put(root);g_free(contents);return;}
    if ((guint64)number(root,"generation")!=v->generation) {json_object_put(root);g_free(contents);return;}
    if (v->snapshot) json_object_put(v->snapshot);
    v->snapshot=root;g_free(v->last_json);v->last_json=contents;
    v->snapshot_time=g_get_monotonic_time();
    if (!v->info) return;
    const char *ensemble=text(root,"radio_ensemble");
    if (!*ensemble) ensemble=text(root,"ensemble");
    update_decoder(v,root,ensemble);
    json_object *services=NULL;json_object_object_get_ex(root,"services",&services);
    char *signature=g_strdup_printf("%s|%s",services ? json_object_to_json_string(services) : "",text(root,"selection"));
    if (g_strcmp0(signature,v->station_signature)) {
        clear(v->stations);g_free(v->station_signature);v->station_signature=g_strdup(signature);
        for (size_t i=0;services && i<json_object_array_length(services);++i) {
            json_object *s=json_object_array_get_idx(services,i);
            guint id=number(s,"id");char sid[24];g_snprintf(sid,sizeof(sid),"0x%X",id);
            char *label=g_strdup_printf("%s%s · %.0f kb/s",!strcmp(sid,text(root,"selection")) ? "▶ " : "",text(s,"label"),number(s,"bitrate"));
            GtkWidget *button=gtk_button_new_with_label(label);g_free(label);
            g_object_set_data(G_OBJECT(button),"service",GUINT_TO_POINTER(id));
            g_signal_connect(button,"clicked",G_CALLBACK(station_clicked),v);
            gtk_box_pack_start(GTK_BOX(v->stations),button,FALSE,FALSE,0);
        }
        gtk_widget_show_all(v->stations);
    }
    g_free(signature);
    unsigned slide=number(root,"slide");
    if (!slide) {gtk_image_clear(GTK_IMAGE(v->image));v->slide=0;}
    else if (slide!=v->slide) {
        path=g_build_filename(v->directory,"slide",NULL);
        GdkPixbuf *pixbuf=gdk_pixbuf_new_from_file_at_scale(path,320,240,TRUE,NULL);g_free(path);
        if (pixbuf) {gtk_image_set_from_pixbuf(GTK_IMAGE(v->image),pixbuf);g_object_unref(pixbuf);v->slide=slide;}
    }
    gtk_label_set_text(GTK_LABEL(v->caption),*text(root,"dynamic_label") ? text(root,"dynamic_label") :
        *text(root,"station") ? text(root,"station") : "Waiting for stations · slideshow appears when broadcast");
    tii_update(v);
}
void dab_view_tick(DabView *v,gboolean active) {
    read_state(v,active);
    if (!v->decoder) return;
    decoder_view_tick(v->decoder,active,"DAB");
    unsigned ueid=(unsigned)dab_view_number(v,"ueid");
    const char *country=dab_country_code(ueid);
    gtk_label_set_text(GTK_LABEL(v->decoder->country),active && *country ? country : "—");
    char identity[160];
    if (active && ueid) g_snprintf(identity,sizeof(identity),"Ensemble country · ECC %02X · EId %04X · Country ID %X",
        ueid>>16,ueid&0xffff,(ueid>>12)&15);
    else g_strlcpy(identity,"Waiting for ensemble ECC and EId",sizeof(identity));
    gtk_widget_set_tooltip_text(v->decoder->country,identity);
    if (active && v->snapshot && g_get_monotonic_time()-v->decoder->status_time<2*G_USEC_PER_SEC) {
        const char *error=dab_view_text(v,"error");
        char *state=g_strdup_printf("DAB · %s · %s",dab_view_text(v,"profile"),
            *error ? error : dab_view_number(v,"tuning") ? "Tuning channel…" :
            !dab_view_number(v,"radio_available") ? "FIC/TII only · audio unavailable" :
            dab_view_number(v,"mux_count")>1 ? "Multiple ensembles · TII attribution withheld" :
            *dab_view_text(v,"route") ? dab_view_text(v,"route") : "Acquiring FIC/TII");
        gtk_label_set_text(GTK_LABEL(v->decoder->state),state);g_free(state);
        gtk_widget_set_tooltip_text(v->decoder->state,dab_view_text(v,"dx_ensembles"));
    }
}
void dab_view_destroy(DabView *v) {
    dab_view_reset(v);
    if (v->directory) {g_rmdir(v->directory);g_clear_pointer(&v->directory,g_free);}
    if (v->tii_dialog) gtk_widget_destroy(v->tii_dialog);
}
