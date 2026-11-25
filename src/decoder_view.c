#define _POSIX_C_SOURCE 200809L
#include "decoder_view.h"
#include "plot_font.h"
#include "decoder_margin.h"
#include <arpa/inet.h>
#include <math.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define STATUS_AGE (10 * G_USEC_PER_SEC)

static gboolean draw_constellation(GtkWidget *widget, cairo_t *cr, gpointer data) {
    DecoderView *v=data;
    double w=gtk_widget_get_allocated_width(widget), h=gtk_widget_get_allocated_height(widget);
    double size=fmin(w,h), left=(w-size)/2, top=(h-size)/2;
    cairo_set_source_rgb(cr,0,0,0); cairo_paint(cr);
    cairo_save(cr);
    cairo_set_antialias(cr,CAIRO_ANTIALIAS_NONE);
    cairo_set_source_rgb(cr,.25,.25,.25); cairo_set_line_width(cr,1);
    cairo_move_to(cr,left+size/2,top); cairo_line_to(cr,left+size/2,top+size);
    cairo_move_to(cr,left,top+size/2); cairo_line_to(cr,left+size,top+size/2);
    cairo_stroke(cr);
    /* Place ideal QPSK symbols (+/-1/sqrt(2)) at the quadrant centres. */
    double scale=size/(4*.70710678);
    cairo_set_source_rgb(cr,.65,.65,.65);
    unsigned order=v->active && v->status.constellation_order==8 ? 8 : 4;
    for (unsigned i=0;i<order && v->status.constellation_order<=8;++i) {
        double angle=2*G_PI*i/order+(order==4 ? G_PI/4 : 0);
        double x=floor(left+size/2+cos(angle)*scale)+.5;
        double y=floor(top+size/2-sin(angle)*scale)+.5;
        cairo_move_to(cr,x-4,y); cairo_line_to(cr,x+4,y);
        cairo_move_to(cr,x,y-4); cairo_line_to(cr,x,y+4);
    }
    cairo_stroke(cr);
    gint64 now=g_get_monotonic_time();
    gboolean fresh=v->active && v->points_time && now-v->points_time<STATUS_AGE;
    if (fresh) {
        cairo_save(cr); cairo_rectangle(cr,left,top,size,size); cairo_clip(cr);
        /* Draw oldest first. A half-second history keeps small updates dense;
         * retain the newest frame across a short pause in incoming telemetry. */
        for (unsigned f=0;f<DECODER_HISTORY_FRAMES;++f) {
            unsigned slot=(v->history_next+f)%DECODER_HISTORY_FRAMES;
            gboolean latest=f==DECODER_HISTORY_FRAMES-1;
            gint64 age=now-v->history_time[slot];
            if (!v->history_time[slot] || (!latest && age>=DECODER_HISTORY_AGE)) continue;
            double alpha=latest ? 1 : .25+.75*(1.-(double)age/DECODER_HISTORY_AGE);
            cairo_set_source_rgba(cr,50./255,205./255,50./255,alpha);
            DecoderConstellation *points=&v->history[slot];
            for (uint32_t i=0;i<points->count;++i) {
                double re=points->iq[2*i], im=points->iq[2*i+1];
                if (!isfinite(re) || !isfinite(im)) continue;
                cairo_rectangle(cr,floor(left+size/2+re*scale),floor(top+size/2-im*scale),1,1);
            }
            cairo_fill(cr);
        }
        cairo_restore(cr);
    }
    cairo_restore(cr);
    plot_font(cr,"sans",CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_source_rgb(cr,.8,.8,.8); cairo_set_font_size(cr,12);
    cairo_move_to(cr,left+6,top+16); cairo_show_text(cr,fresh ? "Q" : "No current constellation");
    cairo_move_to(cr,left+size-14,top+size/2-6); cairo_show_text(cr,"I");
    return FALSE;
}

static GtkWidget *label(GtkWidget *box, const char *text) {
    GtkWidget *l=gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l),0);
    gtk_label_set_ellipsize(GTK_LABEL(l),PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(box),l,FALSE,FALSE,0);
    return l;
}

static GtkWidget *field(GtkWidget *grid, int column, int row, const char *name,
                        const char *unit, gboolean numeric) {
    GtkWidget *caption=gtk_label_new(name), *value=gtk_label_new("—");
    gtk_style_context_add_class(gtk_widget_get_style_context(caption),"decoder-caption");
    gtk_style_context_add_class(gtk_widget_get_style_context(value),"decoder-value");
    gtk_label_set_xalign(GTK_LABEL(caption),0);
    gtk_grid_attach(GTK_GRID(grid),caption,column,row,1,1);
    gtk_label_set_xalign(GTK_LABEL(value),numeric ? 1 : 0);
    /* Bound both size requests so changing digits or missing data cannot
     * move neighbouring fields. Long values remain available as tooltips. */
    gtk_label_set_ellipsize(GTK_LABEL(value),PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(value),10);
    gtk_label_set_max_width_chars(GTK_LABEL(value),10);
    if (numeric) {
        PangoAttrList *attrs=pango_attr_list_new();
        pango_attr_list_insert(attrs,pango_attr_family_new("monospace"));
        gtk_label_set_attributes(GTK_LABEL(value),attrs);
        pango_attr_list_unref(attrs);
    }
    gtk_grid_attach(GTK_GRID(grid),value,column+1,row,1,1);
    GtkWidget *units=gtk_label_new(unit);
    gtk_style_context_add_class(gtk_widget_get_style_context(units),"decoder-unit");
    gtk_label_set_xalign(GTK_LABEL(units),0);
    gtk_grid_attach(GTK_GRID(grid),units,column+2,row,1,1);
    g_object_set_data(G_OBJECT(value),"caption",caption);
    g_object_set_data(G_OBJECT(value),"units",units);
    return value;
}

static GtkWidget *name_field(GtkWidget *grid, int row, const char *name) {
    GtkWidget *caption=gtk_label_new(name), *value=gtk_label_new("—");
    gtk_style_context_add_class(gtk_widget_get_style_context(caption),"decoder-caption");
    gtk_style_context_add_class(gtk_widget_get_style_context(value),"decoder-value");
    gtk_label_set_xalign(GTK_LABEL(caption),0);
    gtk_label_set_xalign(GTK_LABEL(value),0);
    gtk_label_set_ellipsize(GTK_LABEL(value),PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(value),12);
    gtk_label_set_max_width_chars(GTK_LABEL(value),12);
    gtk_grid_attach(GTK_GRID(grid),caption,0,row,1,1);
    gtk_grid_attach(GTK_GRID(grid),value,1,row,5,1);
    g_object_set_data(G_OBJECT(value),"caption",caption);
    return value;
}

GtkWidget *decoder_view_create(DecoderView *v) {
    v->fd=-1;
    GtkWidget *row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    GtkWidget *square=gtk_aspect_frame_new("Constellation",0,0,1,FALSE);
    gtk_widget_set_size_request(square,228,248);
    gtk_widget_set_valign(square,GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row),square,FALSE,FALSE,0);
    v->area=gtk_drawing_area_new();
    gtk_widget_set_size_request(v->area,220,220);
    gtk_container_add(GTK_CONTAINER(square),v->area);
    g_signal_connect(v->area,"draw",G_CALLBACK(draw_constellation),v);
    GtkWidget *frame=gtk_frame_new("Service info"), *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);
    v->service_frame=frame;
    gtk_container_set_border_width(GTK_CONTAINER(box),8);
    gtk_container_add(GTK_CONTAINER(frame),box);
    gtk_box_pack_start(GTK_BOX(row),frame,FALSE,FALSE,0);
    v->state=label(box,"");
    GtkWidget *lamps=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,18);
    gtk_widget_set_halign(lamps,GTK_ALIGN_START);
    gtk_box_set_homogeneous(GTK_BOX(lamps),TRUE);
    gtk_box_pack_start(GTK_BOX(box),lamps,FALSE,FALSE,0);
    for (int i=0;i<4;++i) {
        v->lamps[i]=label(lamps,"");
        gtk_label_set_width_chars(GTK_LABEL(v->lamps[i]),9);
        gtk_label_set_max_width_chars(GTK_LABEL(v->lamps[i]),9);
        gtk_box_set_child_packing(GTK_BOX(lamps),v->lamps[i],TRUE,TRUE,0,GTK_PACK_START);
    }
    GtkWidget *grid=gtk_grid_new();
    gtk_widget_set_name(grid,"decoder-grid");
    gtk_widget_set_halign(grid,GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box),grid,FALSE,FALSE,0);
    v->snr=field(grid,0,0,"SNR","dB",TRUE);
    v->pilot=field(grid,3,0,"Pilot","%",TRUE);
    v->cfo=field(grid,0,1,"CFO","Hz",TRUE);
    v->buffer=field(grid,3,1,"Buffer","%",TRUE);
    v->rms=field(grid,0,2,"IQ RMS","dBFS",TRUE);
    v->peak=field(grid,3,2,"Peak","dBFS",TRUE);
    gtk_widget_set_tooltip_text(v->buffer,"OFDM → FEC queue fill level; excludes the task currently being processed.");
    v->service=name_field(grid,3,"Service");
    v->provider=name_field(grid,4,"Provider");
    v->modulation=name_field(grid,5,"Mode");
    v->video_codec=field(grid,0,6,"Video codec","",FALSE);
    v->audio_codec=field(grid,3,6,"Audio codec","",FALSE);
    v->fec=field(grid,0,7,"FEC","",FALSE);
    v->guard=field(grid,3,7,"GI","",FALSE);
    v->packets=field(grid,0,8,"TS","",TRUE);
    v->written=field(grid,3,8,"Output","",TRUE);
    v->rs_ok=field(grid,0,9,"RS OK","",TRUE);
    v->rs_errors=field(grid,3,9,"RS errors","",TRUE);
    v->cc_errors=field(grid,0,10,"CC errors","",TRUE);
    v->margin=field(grid,3,10,"D margin","dB",TRUE);
    v->country=name_field(grid,11,"Country");
    gtk_widget_set_no_show_all(v->country,TRUE);
    gtk_widget_set_no_show_all(g_object_get_data(G_OBJECT(v->country),"caption"),TRUE);
    return row;
}

void decoder_view_stop(DecoderView *v) {
    if (v->fd>=0 && v->port) close(v->fd);
    v->fd=-1; v->port=0; v->active=FALSE;
    v->service_valid=FALSE; v->selected_service[0]=v->selected_provider[0]=0;
    v->selected_video_codec[0]=v->selected_audio_codec[0]=0;
    v->status_time=v->points_time=v->buffer_time=v->health_time=0;
    memset(v->history_time,0,sizeof(v->history_time));
    v->history_next=0;
    memset(&v->queue,0,sizeof(v->queue));
    memset(&v->status,0,sizeof(v->status)); memset(&v->points,0,sizeof(v->points));
}

gboolean decoder_view_start(DecoderView *v) {
    decoder_view_stop(v);
    int fd=socket(AF_INET,SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,0);
    if (fd<0) return FALSE;
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    socklen_t size=sizeof(addr);
    if (bind(fd,(struct sockaddr *)&addr,size)<0 || getsockname(fd,(struct sockaddr *)&addr,&size)<0) {
        close(fd); return FALSE;
    }
    v->fd=fd; v->port=ntohs(addr.sin_port);
    return TRUE;
}

static void receive_frames(DecoderView *v) {
    if (v->fd<0 || !v->port) return;
    union { DecoderStatus status; DecoderConstellation points; DecoderBuffer queue; DecoderDvbtHealth health; } frame;
    /* Bound GUI work even if a backend produces too many updates. MSG_TRUNC
     * ensures oversized datagrams cannot masquerade as a valid frame. */
    for (int i=0;i<64;++i) {
        ssize_t n=recv(v->fd,&frame,sizeof(frame),MSG_DONTWAIT | MSG_TRUNC);
        if (n<0) break;
        if (n<8 || frame.status.magic!=DECODER_MONITOR_MAGIC || !v->active) continue;
        if (frame.status.type==DECODER_STATUS && n==sizeof(DecoderStatus)) {
            if (v->hardware_ts && frame.status.satellite) {
                /* The USB relay owns TS counters and service lock. The RF
                 * monitor cannot observe either at its demodulator registers. */
                unsigned mask=DECODER_TS|DECODER_SERVICE;
                frame.status.locks=(frame.status.locks & ~mask)|(v->status.locks & mask);
                frame.status.lock_valid=(frame.status.lock_valid & ~mask)|(v->status.lock_valid & mask);
                frame.status.packets=v->status.packets;
                frame.status.written_packets=v->status.written_packets;
                g_strlcpy(frame.status.service,v->status.service,sizeof(frame.status.service));
                g_strlcpy(frame.status.provider,v->status.provider,sizeof(frame.status.provider));
            }
            v->status=frame.status;
            v->status.service[sizeof(v->status.service)-1]=0;
            v->status.provider[sizeof(v->status.provider)-1]=0;
            v->status.modulation[sizeof(v->status.modulation)-1]=0;
            v->status.fec[sizeof(v->status.fec)-1]=0;
            v->status.guard[sizeof(v->status.guard)-1]=0;
            v->status_time=g_get_monotonic_time();
        } else if (frame.points.type==DECODER_CONSTELLATION && n==sizeof(DecoderConstellation) && frame.points.count<=DECODER_POINTS) {
            v->points=frame.points; v->points_time=g_get_monotonic_time();
            v->points_received_us=g_get_real_time();
            v->history[v->history_next]=frame.points;
            v->history_time[v->history_next]=v->points_time;
            v->history_next=(v->history_next+1)%DECODER_HISTORY_FRAMES;
        } else if (frame.health.type==DECODER_DVBT_HEALTH && n==sizeof(DecoderDvbtHealth)) {
            v->health=frame.health; v->health.outer_state[sizeof(v->health.outer_state)-1]=0;
            v->health_time=g_get_monotonic_time();
            v->health_received_us=g_get_real_time();
        } else if (frame.queue.type==DECODER_BUFFER && n==sizeof(DecoderBuffer)) {
            v->queue=frame.queue; v->buffer_time=g_get_monotonic_time();
        }
    }
}

static void set_value(GtkWidget *widget, const char *value) {
    char *valid=g_utf8_make_valid(value,-1);
    gtk_label_set_text(GTK_LABEL(widget),*valid ? valid : "—");
    gtk_widget_set_tooltip_text(widget,*valid ? valid : NULL);
    g_free(valid);
}

static void measurement(GtkWidget *widget, const char *format, double value) {
    char text[32];
    if (isfinite(value)) g_snprintf(text,32,format,value);
    else g_strlcpy(text,"—",32);
    set_value(widget,text);
}

static void update_margin(DecoderView *v, gboolean fresh, const char *mode) {
    const DecoderStatus *s=&v->status;
    gboolean matches=(s->satellite==1 && !strcmp(mode,"DVB-S")) ||
                     (s->satellite==2 && !strcmp(mode,"DVB-S2"));
    double margin=fresh && matches ? decoder_margin_db(s) : NAN;
    char text[768];
    if (isfinite(margin)) {
        /* Round before colouring so a displayed zero never appears negative. */
        double shown=round(margin*10)/10;
        if (shown==0) shown=0; /* Avoid -0.0. */
        const char *color=shown<0 ? "#e05a55" : shown<2 ? "#e8b44c" : "#36d866";
        g_snprintf(text,sizeof(text),"<span foreground=\"%s\" weight=\"bold\">%+.1f</span>",color,shown);
        gtk_label_set_markup(GTK_LABEL(v->margin),text);
        g_snprintf(text,sizeof(text),
            "Estimated MER reserve: %.1f dB MER minus %.2f dB reference.\n"
            "%s%s\nRed: below 0 dB; amber: 0 to 2 dB; green: at least 2 dB.\n"
            "A signal report estimate, not a calibrated decoding limit; fading and interference can still cause errors.",
            s->mer_db,decoder_margin_reference(s),
            s->satellite==2 ? "DVB-S2 AWGN reference (ETSI EN 302 307-1, table 13)." :
                             "DVB-S reference (ETSI EN 300 421, table 3, converted to Es/N0).",
            s->satellite==2 && s->short_frame ? " Short frame: approximate +0.30 dB allowance." : "");
    } else {
        gtk_label_set_text(GTK_LABEL(v->margin),"—");
        g_snprintf(text,sizeof(text),!strcmp(mode,"DVB-T") ?
            "D margin is available for DVB-S/S2; no DVB-T reference is used." :
            "Estimated MER reserve. Requires current MER, sync/PL lock and a supported modulation/FEC and frame size.");
    }
    gtk_widget_set_tooltip_text(v->margin,text);
}

void decoder_view_tick(DecoderView *v, gboolean active, const char *mode) {
    v->active=active;
    receive_frames(v);
    if (!v->area) return;
    gboolean fresh=active && v->status_time && g_get_monotonic_time()-v->status_time<(v->hardware_ts ? 2*G_USEC_PER_SEC : STATUS_AGE);
    DecoderStatus *s=&v->status;
    gboolean satellite=!strcmp(mode,"DVB-S") || !strcmp(mode,"DVB-S2");
    gboolean dab=!strcmp(mode,"DAB");
    gtk_widget_set_visible(v->country,dab);
    gtk_widget_set_visible(g_object_get_data(G_OBJECT(v->country),"caption"),dab);
    gboolean t2=!strcmp(mode,"DVB-T2");
    gboolean s2=!strcmp(mode,"DVB-S2");
    update_margin(v,fresh,mode);
    GtkWidget *fields[]={v->service,v->provider,v->pilot,v->buffer,v->video_codec,v->packets,v->written,v->rs_ok,v->rs_errors,v->cc_errors,v->margin};
    const char *dab_names[]={"Ensemble","Station","CP lock","Capture","Slideshow","FIB OK","Audio","FIGs","FIB errors","IQ drops","MER"};
    const char *dvb_names[]={"Service","Provider","Pilot","Buffer","Video codec","TS","Output","RS OK","RS errors","CC errors","D margin"};
    for (unsigned i=0;i<G_N_ELEMENTS(fields);++i)
        gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(fields[i]),"caption")),dab ? dab_names[i] : dvb_names[i]);
    if (dab) measurement(v->margin,"%.1f",fresh ? s->mer_db : NAN);
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(v->snr),"caption")),satellite ? "MER" : "SNR");
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(v->pilot),"caption")),dab ? "CP lock" : satellite ? "Pilots" : "Pilot");
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(v->pilot),"units")),satellite ? "" : "%");
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(v->guard),"caption")),satellite ? "Frame" : "GI");
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(v->rs_ok),"caption")),dab ? "FIGs" : (s2 || t2) ? "BCH OK" : "RS OK");
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(v->rs_errors),"caption")),dab ? "FIB errors" : (s2 || t2) ? "BCH errors" : "RS errors");
    gtk_widget_set_tooltip_text(v->buffer,dab ? "Progress of the current 8-second FIC/TII capture" : t2 ? "Live IQ queue fill level (loss counters above)" : satellite ? "Not reported by this decoder" : "OFDM → FEC queue fill level");
    if ((satellite || t2) && !v->hardware_ts) {
        s->lock_valid |= DECODER_SERVICE;
        if (v->service_valid) s->locks |= DECODER_SERVICE; else s->locks &= ~DECODER_SERVICE;
        g_strlcpy(s->service,v->selected_service,sizeof(s->service));
        g_strlcpy(s->provider,v->selected_provider,sizeof(s->provider));
    }
    char text[512];
    g_snprintf(text,sizeof(text),"%s · %s",mode,!active ? "stopped" : fresh ? "live" : "waiting for decoder status");
    if (t2 && fresh) g_snprintf(text,sizeof(text),"DVB-T2 · PLP %d · LDPC: %s · IQ lost: %" G_GUINT64_FORMAT " · FEC frames lost: %" G_GUINT64_FORMAT,
        s->plp_id,s->ldpc_backend==2 ? "GPU" : s->ldpc_backend==1 ?
            (s->ldpc_gpu_requested ? "CPU (fallback)" : "CPU") : "waiting",s->input_dropped,s->fec_frames_dropped);
    gtk_label_set_text(GTK_LABEL(v->state),text);
    const char *names[]={dab ? "Sync" : satellite ? (s2 ? "PL lock" : "Sync") : "OFDM", dab ? "FIC" : "FEC", dab ? "Ensemble" : "TS", dab ? "Audio" : "Service"};
    for (int i=0;i<4;++i) {
        unsigned lock=dab && i>=2 ? (i==2 ? DECODER_SERVICE : DECODER_TS) : (1u<<i);
        const char *color=(!fresh || ((satellite || dab || v->hardware_ts) && !(s->lock_valid & lock))) ? "#888888" : (s->locks & lock) ? "#36d866" : "#e05a55";
        g_snprintf(text,sizeof(text),"<span foreground=\"%s\">●</span> %s",color,names[i]);
        gtk_label_set_markup(GTK_LABEL(v->lamps[i]),text);
    }
    if (fresh) {
        measurement(v->snr,"%.1f",satellite ? s->mer_db : s->snr_db);
        if (satellite) set_value(v->pilot,s2 && s->modcod>0 ? (s->pilots ? "on" : "off") : "");
        else measurement(v->pilot,"%.0f",100*s->pilot_lock);
        measurement(v->cfo,"%+.1f",s->cfo_hz);
        measurement(v->rms,"%.1f",s->rms_dbfs); measurement(v->peak,"%.1f",s->peak_dbfs);
        set_value(v->service,s->service); set_value(v->provider,s->provider);
        if (s->buffer_valid && isfinite(s->buffer_percent))
            g_snprintf(text,sizeof(text),"%.0f",CLAMP(s->buffer_percent,0,100));
        else g_snprintf(text,sizeof(text),"—");
        gtk_label_set_text(GTK_LABEL(v->buffer),text);
        set_value(v->modulation,s->modulation); set_value(v->fec,s->fec);
        set_value(v->guard,satellite ? (s2 && s->modcod>0 ? (s->short_frame ? "short" : "normal") : "") : s->guard);
        measurement(v->packets,"%.0f",s->packets); measurement(v->written,"%.0f",s->written_packets);
        measurement(v->rs_ok,"%.0f",s->rs_ok); measurement(v->rs_errors,"%.0f",s->rs_uncorrectable);
        measurement(v->cc_errors,"%.0f",s->cc_errors);
        if (v->hardware_ts) {
            set_value(v->rs_ok,""); set_value(v->rs_errors,""); set_value(v->cc_errors,"");
        }
        if (satellite && !v->hardware_ts) {
            measurement(v->rs_ok,"%.0f",s->fec_good); measurement(v->rs_errors,"%.0f",s->fec_bad);
            set_value(v->cc_errors,""); set_value(v->written,"");
        }
    } else {
        GtkWidget *fields[]={v->snr,v->pilot,v->cfo,v->rms,v->peak,v->service,v->provider,
            v->modulation,v->fec,v->guard,v->packets,v->written,v->rs_ok,v->rs_errors,v->cc_errors};
        for (size_t i=0;i<G_N_ELEMENTS(fields);++i) set_value(fields[i],"");
        gtk_label_set_text(GTK_LABEL(v->buffer),"—");
    }
    if (active && v->buffer_time && g_get_monotonic_time()-v->buffer_time<STATUS_AGE && v->queue.capacity) {
        g_snprintf(text,sizeof(text),"%.0f",CLAMP(100. * v->queue.queued/v->queue.capacity,0,100));
        gtk_label_set_text(GTK_LABEL(v->buffer),text);
    }
    /* Service metadata remains useful through brief losses of decoder lock. */
    set_value(v->video_codec,active ? v->selected_video_codec : "");
    set_value(v->audio_codec,active ? v->selected_audio_codec : "");
    gtk_widget_queue_draw(v->area);
}
