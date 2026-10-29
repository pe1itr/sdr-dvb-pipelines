#define _POSIX_C_SOURCE 200809L
#include "decoder_view.h"
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
    /* Match the original receiver's scale and ideal QPSK reference crosses. */
    double scale=size*.43/1.6;
    cairo_set_source_rgb(cr,.65,.65,.65);
    for (int re=-1;re<=1;re+=2) for (int im=-1;im<=1;im+=2) {
        double x=floor(left+size/2+re*.70710678*scale)+.5;
        double y=floor(top+size/2-im*.70710678*scale)+.5;
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
    cairo_set_source_rgb(cr,.8,.8,.8); cairo_set_font_size(cr,12);
    cairo_move_to(cr,left+6,top+16); cairo_show_text(cr,fresh ? "Q" : "Geen actuele constellatie");
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
    return value;
}

GtkWidget *decoder_view_create(DecoderView *v) {
    v->fd=-1;
    GtkWidget *row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    GtkWidget *square=gtk_aspect_frame_new("Constellatie",0,0,1,FALSE);
    gtk_widget_set_size_request(square,228,248);
    gtk_widget_set_valign(square,GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row),square,FALSE,FALSE,0);
    v->area=gtk_drawing_area_new();
    gtk_widget_set_size_request(v->area,220,220);
    gtk_container_add(GTK_CONTAINER(square),v->area);
    g_signal_connect(v->area,"draw",G_CALLBACK(draw_constellation),v);
    GtkWidget *frame=gtk_frame_new("Service-info"), *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);
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
    v->peak=field(grid,3,2,"Piek","dBFS",TRUE);
    gtk_widget_set_tooltip_text(v->buffer,"Vulling van de OFDM → FEC-wachtrij; de taak die verwerkt wordt telt niet mee.");
    v->service=name_field(grid,3,"Service");
    v->provider=name_field(grid,4,"Provider");
    v->modulation=name_field(grid,5,"Mode");
    v->fec=field(grid,0,6,"FEC","",FALSE);
    v->guard=field(grid,3,6,"GI","",FALSE);
    v->packets=field(grid,0,7,"TS","",TRUE);
    v->written=field(grid,3,7,"Uit","",TRUE);
    v->rs_ok=field(grid,0,8,"RS goed","",TRUE);
    v->rs_errors=field(grid,3,8,"RS fout","",TRUE);
    v->cc_errors=field(grid,0,9,"CC-fouten","",TRUE);
    GtkWidget *empty=gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(empty),"decoder-caption");
    gtk_grid_attach(GTK_GRID(grid),empty,3,9,3,1);
    return row;
}

void decoder_view_stop(DecoderView *v) {
    if (v->fd>=0 && v->port) close(v->fd);
    v->fd=-1; v->port=0; v->active=FALSE;
    v->status_time=v->points_time=v->buffer_time=0;
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
    union { DecoderStatus status; DecoderConstellation points; DecoderBuffer queue; } frame;
    /* Bound GUI work even if a backend produces too many updates. MSG_TRUNC
     * ensures oversized datagrams cannot masquerade as a valid frame. */
    for (int i=0;i<64;++i) {
        ssize_t n=recv(v->fd,&frame,sizeof(frame),MSG_DONTWAIT | MSG_TRUNC);
        if (n<0) break;
        if (n<8 || frame.status.magic!=DECODER_MONITOR_MAGIC || !v->active) continue;
        if (frame.status.type==DECODER_STATUS && n==sizeof(DecoderStatus)) {
            v->status=frame.status;
            v->status.service[sizeof(v->status.service)-1]=0;
            v->status.provider[sizeof(v->status.provider)-1]=0;
            v->status.modulation[sizeof(v->status.modulation)-1]=0;
            v->status.fec[sizeof(v->status.fec)-1]=0;
            v->status.guard[sizeof(v->status.guard)-1]=0;
            v->status_time=g_get_monotonic_time();
        } else if (frame.points.type==DECODER_CONSTELLATION && n==sizeof(DecoderConstellation) && frame.points.count<=DECODER_POINTS) {
            v->points=frame.points; v->points_time=g_get_monotonic_time();
            v->history[v->history_next]=frame.points;
            v->history_time[v->history_next]=v->points_time;
            v->history_next=(v->history_next+1)%DECODER_HISTORY_FRAMES;
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

void decoder_view_tick(DecoderView *v, gboolean active, const char *mode) {
    v->active=active;
    receive_frames(v);
    if (!v->area) return;
    gboolean fresh=active && v->status_time && g_get_monotonic_time()-v->status_time<STATUS_AGE;
    DecoderStatus *s=&v->status;
    gboolean supported=!strcmp(mode,"DVB-T");
    char text[512];
    g_snprintf(text,sizeof(text),"%s · %s",mode,!supported ? "ingebouwde weergave volgt" :
        !active ? "gestopt" : fresh ? "live" : "wachten op decoderstatus");
    gtk_label_set_text(GTK_LABEL(v->state),text);
    const char *names[]={"OFDM", "FEC", "TS", "Service"};
    for (int i=0;i<4;++i) {
        const char *color=!fresh ? "#888888" : (s->locks & (1u<<i)) ? "#36d866" : "#e05a55";
        g_snprintf(text,sizeof(text),"<span foreground=\"%s\">●</span> %s",color,names[i]);
        gtk_label_set_markup(GTK_LABEL(v->lamps[i]),text);
    }
    if (fresh) {
        measurement(v->snr,"%.1f",s->snr_db); measurement(v->pilot,"%.0f",100*s->pilot_lock);
        measurement(v->cfo,"%+.1f",s->cfo_hz);
        measurement(v->rms,"%.1f",s->rms_dbfs); measurement(v->peak,"%.1f",s->peak_dbfs);
        set_value(v->service,s->service); set_value(v->provider,s->provider);
        if (s->buffer_valid && isfinite(s->buffer_percent))
            g_snprintf(text,sizeof(text),"%.0f",CLAMP(s->buffer_percent,0,100));
        else g_snprintf(text,sizeof(text),"—");
        gtk_label_set_text(GTK_LABEL(v->buffer),text);
        set_value(v->modulation,s->modulation); set_value(v->fec,s->fec); set_value(v->guard,s->guard);
        measurement(v->packets,"%.0f",s->packets); measurement(v->written,"%.0f",s->written_packets);
        measurement(v->rs_ok,"%.0f",s->rs_ok); measurement(v->rs_errors,"%.0f",s->rs_uncorrectable);
        measurement(v->cc_errors,"%.0f",s->cc_errors);
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
    gtk_widget_queue_draw(v->area);
}
