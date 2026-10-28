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
    cairo_set_source_rgb(cr,.25,.25,.25); cairo_set_line_width(cr,1);
    for (int i=1;i<4;++i) {
        double offset=size*i/4;
        cairo_move_to(cr,left+offset,top); cairo_line_to(cr,left+offset,top+size);
        cairo_move_to(cr,left,top+offset); cairo_line_to(cr,left+size,top+offset);
    }
    cairo_stroke(cr);
    gboolean fresh=v->active && v->points_time && g_get_monotonic_time()-v->points_time<STATUS_AGE;
    if (fresh) {
        cairo_save(cr); cairo_rectangle(cr,left,top,size,size); cairo_clip(cr);
        cairo_set_source_rgba(cr,.25,1,.45,.8);
        for (uint32_t i=0;i<v->points.count;++i) {
            double re=v->points.iq[2*i], im=v->points.iq[2*i+1];
            if (!isfinite(re) || !isfinite(im)) continue;
            cairo_rectangle(cr,left+size*(.5+re/4)-1,top+size*(.5-im/4)-1,2,2);
        }
        cairo_fill(cr); cairo_restore(cr);
    }
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
    gtk_container_set_border_width(GTK_CONTAINER(box),8);
    gtk_container_add(GTK_CONTAINER(frame),box);
    gtk_box_pack_start(GTK_BOX(row),frame,TRUE,TRUE,0);
    v->state=label(box,"");
    GtkWidget *lamps=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,18);
    gtk_box_pack_start(GTK_BOX(box),lamps,FALSE,FALSE,0);
    for (int i=0;i<4;++i) v->lamps[i]=label(lamps,"");
    v->signal=label(box,"");
    v->service=label(box,"Service: —");
    v->provider=label(box,"Provider: —");
    v->buffer=label(box,"Buffer: —");
    gtk_widget_set_tooltip_text(v->buffer,"Vulling van de OFDM → FEC-wachtrij; de taak die verwerkt wordt telt niet mee.");
    v->details=label(box,"");
    return row;
}

void decoder_view_stop(DecoderView *v) {
    if (v->fd>=0 && v->port) close(v->fd);
    v->fd=-1; v->port=0; v->active=FALSE;
    v->status_time=v->points_time=v->buffer_time=0;
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
        } else if (frame.queue.type==DECODER_BUFFER && n==sizeof(DecoderBuffer)) {
            v->queue=frame.queue; v->buffer_time=g_get_monotonic_time();
        }
    }
}

static void set_name(GtkWidget *widget, const char *prefix, const char *value) {
    char *valid=g_utf8_make_valid(value,-1), *text=g_strdup_printf("%s: %s",prefix,*valid ? valid : "—");
    gtk_label_set_text(GTK_LABEL(widget),text);
    gtk_widget_set_tooltip_text(widget,text);
    g_free(valid); g_free(text);
}

static void measurement(char text[32], const char *format, double value) {
    if (isfinite(value)) g_snprintf(text,32,format,value);
    else g_strlcpy(text,"—",32);
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
        char snr[32], pilot[32], cfo[32], rms[32], peak[32];
        measurement(snr,"%.1f",s->snr_db); measurement(pilot,"%.0f",100*s->pilot_lock);
        measurement(cfo,"%+.1f",s->cfo_hz); measurement(rms,"%.1f",s->rms_dbfs); measurement(peak,"%.1f",s->peak_dbfs);
        g_snprintf(text,sizeof(text),"SNR %s dB · Pilot %s%% · CFO %s Hz\nIQ RMS %s dBFS · Piek %s dBFS",
            snr,pilot,cfo,rms,peak);
        gtk_label_set_text(GTK_LABEL(v->signal),text);
        set_name(v->service,"Service",s->service); set_name(v->provider,"Provider",s->provider);
        if (s->buffer_valid && isfinite(s->buffer_percent))
            g_snprintf(text,sizeof(text),"Buffer: %.0f%%",CLAMP(s->buffer_percent,0,100));
        else g_snprintf(text,sizeof(text),"Buffer: —");
        gtk_label_set_text(GTK_LABEL(v->buffer),text);
        g_snprintf(text,sizeof(text),"%s · FEC %s · GI %s\nTS %u · Uit %u · RS goed %u / fout %u · CC-fouten %u",
            s->modulation,s->fec,s->guard,s->packets,s->written_packets,s->rs_ok,s->rs_uncorrectable,s->cc_errors);
        char *valid=g_utf8_make_valid(text,-1); gtk_label_set_text(GTK_LABEL(v->details),valid); g_free(valid);
    } else {
        gtk_label_set_text(GTK_LABEL(v->signal),"SNR — · Pilot — · CFO —\nIQ RMS — · Piek —");
        set_name(v->service,"Service",""); set_name(v->provider,"Provider","");
        gtk_label_set_text(GTK_LABEL(v->buffer),"Buffer: —");
        gtk_label_set_text(GTK_LABEL(v->details),"");
    }
    if (active && v->buffer_time && g_get_monotonic_time()-v->buffer_time<STATUS_AGE && v->queue.capacity) {
        g_snprintf(text,sizeof(text),"Buffer: %.0f%%",CLAMP(100. * v->queue.queued/v->queue.capacity,0,100));
        gtk_label_set_text(GTK_LABEL(v->buffer),text);
    }
    gtk_widget_queue_draw(v->area);
}
