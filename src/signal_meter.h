#include "plot_font.h"
#ifndef SIGNAL_METER_H
#define SIGNAL_METER_H
#include <gtk/gtk.h>
#include <math.h>
#include "iq_monitor.h"

#define SIGNAL_METER_POINTS 4096
typedef struct {
    GtkWidget *area, *buttons[6];
    double reference, span, interval;
    double center, rate, bandwidth, offset;
    gboolean filtered, active, configured;
    struct { double time, db; } points[SIGNAL_METER_POINTS];
    unsigned head, count, average_count;
    double sum, bucket, origin, last, value;
} SignalMeter;

/* Integrate linear PSD over Hz, not logarithmic bins. The Nyquist bin spans
 * both ends of the FFT-shifted band; fractional edge bins retain their width. */
static double signal_meter_power(const IQMonitorFrame *frame, gboolean filtered,
                                 double bandwidth, double offset) {
    double rate=frame->rate;
    if (!isfinite(rate) || rate<=0) return NAN;
    double low=-rate/2, high=rate/2, bin=rate/IQ_MONITOR_BINS;
    if (filtered) {
        if (!isfinite(bandwidth) || bandwidth<=0 || !isfinite(offset)) return NAN;
        low=MAX(low,offset-bandwidth/2); high=MIN(high,offset+bandwidth/2);
    }
    if (high<=low) return NAN;
    double power=0;
    for (int i=0; i<IQ_MONITOR_BINS; ++i) {
        if (!isfinite(frame->power[i])) return NAN;
        double f=-rate/2+i*bin;
        double width=MAX(0,MIN(high,f+bin/2)-MAX(low,f-bin/2));
        if (!i) width+=MAX(0,high-MAX(low,rate/2-bin/2));
        power+=pow(10,frame->power[i]/10.0)*width;
    }
    return power;
}

static void signal_meter_reset(SignalMeter *m) {
    m->head=m->count=m->average_count=0;
    m->sum=m->bucket=m->origin=m->last=0;
    m->value=NAN;
}

static void signal_meter_configure(SignalMeter *m, gboolean active, gboolean filtered,
                                    double bandwidth, double offset, double center, double rate) {
    if (!m->configured || (active && !m->active) || filtered!=m->filtered ||
        center!=m->center || rate!=m->rate ||
        (filtered && (bandwidth!=m->bandwidth || offset!=m->offset)))
        signal_meter_reset(m);
    m->active=active; m->filtered=filtered; m->bandwidth=bandwidth;
    m->offset=offset; m->center=center; m->rate=rate; m->configured=TRUE;
}

static void signal_meter_feed(SignalMeter *m, const IQMonitorFrame *frame, double now) {
    if (!m->active) return;
    double power=signal_meter_power(frame,m->filtered,m->bandwidth,m->offset);
    if (!isfinite(power) || power<0) return;
    if (!m->average_count) m->origin=now;
    double bucket=floor((now-m->origin)/m->interval+1e-8);
    if (m->average_count && (bucket!=m->bucket || now-m->last>1.5)) {
        m->points[m->head].time=m->last;
        m->points[m->head].db=m->value;
        m->head=(m->head+1)%SIGNAL_METER_POINTS;
        m->count=MIN(m->count+1,SIGNAL_METER_POINTS);
        m->sum=0; m->average_count=0;
    }
    m->bucket=bucket; m->sum+=power; ++m->average_count; m->last=now;
    m->value=10*log10(MAX(m->sum/m->average_count,1e-20));
}

static gboolean signal_meter_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    SignalMeter *m=data;
    double w=gtk_widget_get_allocated_width(widget), h=gtk_widget_get_allocated_height(widget);
    double left=56, right=w-5, top=25, bottom=h-27;
    if (right<=left || bottom<=top) return FALSE;
    cairo_set_source_rgb(cr,0,0,0); cairo_paint(cr);
    cairo_set_line_width(cr,1);
    cairo_set_source_rgb(cr,.66,.66,.66);
    cairo_rectangle(cr,.5,.5,w-1,h-1); cairo_stroke(cr);
    plot_font(cr,"monospace",CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr,11);
    char text[128];
    double floor_db=m->reference-m->span/2;
    /* Linrad uses horizontal grid lines, a left dB scale and a yellow RMS
     * trace. The six arrows occupy the same corners as meter_graph.c. */
    double raw=m->span/5, decade=pow(10,floor(log10(raw)));
    double step=(raw/decade<=1 ? 1 : raw/decade<=2 ? 2 : raw/decade<=5 ? 5 : 10)*decade;
    for (double db=ceil(floor_db/step)*step; db<=floor_db+m->span; db+=step) {
        double y=bottom-(db-floor_db)/m->span*(bottom-top);
        cairo_set_source_rgb(cr,0,22.0/63,29.0/63);
        cairo_move_to(cr,left,y+.5); cairo_line_to(cr,right,y+.5); cairo_stroke(cr);
        cairo_set_source_rgb(cr,.75,.75,.75);
        g_snprintf(text,sizeof(text),step<1 ? "%.1f" : "%.0f",db);
        cairo_move_to(cr,3,y+4); cairo_show_text(cr,text);
    }
    double duration=(right-left)*m->interval;
    double now=g_get_monotonic_time()/1e6;
    gboolean fresh=m->active && m->average_count && now-m->last<=1.5;
    double elapsed=m->average_count ? MAX(0,(m->active ? now : m->last)-m->origin) : 0;
    /* Like Linrad, write left to right, then move the old trace back 20%. */
    double start=elapsed>duration ? ceil((elapsed-duration)/(duration*.2))*duration*.2 : 0;
    cairo_save(cr); cairo_rectangle(cr,left,top,right-left,bottom-top); cairo_clip(cr);
    cairo_set_source_rgb(cr,1,1,1.0/3);
    gboolean connected=FALSE; double previous=0;
    for (unsigned j=0; j<m->count+(m->average_count ? 1u : 0u); ++j) {
        unsigned i=(m->head+SIGNAL_METER_POINTS-m->count+j)%SIGNAL_METER_POINTS;
        double time=j==m->count ? m->last : m->points[i].time;
        double db=j==m->count ? m->value : m->points[i].db;
        double x=left+(time-m->origin-start)/m->interval;
        double y=bottom-CLAMP((db-floor_db)/m->span,0,1)*(bottom-top);
        if (!connected || time-previous>MAX(1.5,m->interval*1.5)) {
            cairo_move_to(cr,x,y); cairo_line_to(cr,x+.5,y);
        } else cairo_line_to(cr,x,y);
        connected=TRUE; previous=time;
    }
    cairo_stroke(cr); cairo_restore(cr);
    cairo_set_source_rgb(cr,.8,.8,.8);
    if (fresh) g_snprintf(text,sizeof(text),"%s  %.1f dBFS",m->filtered ? "BW" : "Full",m->value);
    else g_snprintf(text,sizeof(text),"%s  %s",m->filtered ? "BW" : "Full",m->active ? "Waiting for IQ" : "Stopped");
    cairo_move_to(cr,28,15); cairo_show_text(cr,text);
    cairo_move_to(cr,28,h-8); cairo_show_text(cr,"dBFS");
    g_snprintf(text,sizeof(text),"%.1f s/pt  %.0f s",m->interval,duration);
    cairo_move_to(cr,80,h-8); cairo_show_text(cr,text);
    return FALSE;
}

static void signal_meter_refresh(SignalMeter *m) {
    const char *tips[]={"Finer level resolution", "Coarser level resolution",
        "Move trace up", "Move trace down", "Faster / less averaging", "Slower / more averaging"};
    gboolean sensitive[]={m->span>1,m->span<160,m->reference>-200,m->reference<20,
                          m->interval>.100001,m->interval<12.799999};
    for (int i=0; i<6; ++i) {
        char *tip=g_strdup_printf("%s · range %.1f dB · %.1f s per point",tips[i],m->span,m->interval);
        gtk_widget_set_tooltip_text(m->buttons[i],tip); g_free(tip);
        gtk_widget_set_sensitive(m->buttons[i],sensitive[i]);
    }
    gtk_widget_queue_draw(m->area);
}

static void signal_meter_clicked(GtkButton *button, gpointer data) {
    SignalMeter *m=data;
    int i=GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button),"meter-index"));
    if (i==0) m->span=MAX(1,m->span/1.3);
    if (i==1) m->span=MIN(160,m->span*1.4);
    if (i==2) m->reference=MAX(-200,m->reference-m->span*.1);
    if (i==3) m->reference=MIN(20,m->reference+m->span*.1);
    if (i>=4) {
        m->interval=CLAMP(m->interval*(i==4 ? .5 : 2),.1,12.8);
        signal_meter_reset(m);
    }
    signal_meter_refresh(m);
}

static gboolean signal_meter_position(GtkOverlay *overlay, GtkWidget *child,
                                       GdkRectangle *rect, gpointer data) {
    (void)overlay; SignalMeter *m=data;
    int w=gtk_widget_get_allocated_width(m->area), h=gtk_widget_get_allocated_height(m->area);
    for (int i=0; i<6; ++i) if (child==m->buttons[i]) {
        GtkRequisition size; gtk_widget_get_preferred_size(child,NULL,&size);
        int x=i<2 ? 3 : i<4 ? w-size.width-3 : w-(i==4 ? 3 : 2)*(size.width+4);
        *rect=(GdkRectangle){x,(i==0 || i==2) ? 3 : h-size.height-3,size.width,size.height};
        return TRUE;
    }
    return FALSE;
}

static double signal_meter_setting(GKeyFile *config, const char *key, double fallback,
                                    double low, double high) {
    GError *error=NULL;
    double value=g_key_file_get_double(config,"display",key,&error);
    if (error || !isfinite(value) || value<low || value>high) value=fallback;
    g_clear_error(&error); return value;
}

static GtkWidget *signal_meter_create(SignalMeter *m, GKeyFile *config) {
    m->reference=signal_meter_setting(config,"meter_reference_dbfs",-50,-200,20);
    m->span=signal_meter_setting(config,"meter_range_db",80,1,160);
    m->interval=signal_meter_setting(config,"meter_seconds_per_point",.1,.1,12.8);
    signal_meter_reset(m);
    GtkWidget *overlay=gtk_overlay_new();
    m->area=gtk_drawing_area_new(); gtk_widget_set_size_request(m->area,430,110);
    gtk_container_add(GTK_CONTAINER(overlay),m->area);
    gtk_widget_set_tooltip_text(m->area,
        "Signal strength over time · yellow: mean power in dBFS. BW on: integrate the selected band at the receiver offset; BW off: full input spectrum. Horizontal arrows change averaging and speed. W saves the display settings.");
    g_signal_connect(m->area,"draw",G_CALLBACK(signal_meter_draw),m);
    const char *icons[]={"pan-up-symbolic","pan-down-symbolic","pan-up-symbolic",
        "pan-down-symbolic","pan-end-symbolic","pan-start-symbolic"};
    const char *names[]={"Finer meter resolution","Coarser meter resolution","Move meter trace up",
        "Move meter trace down","Faster signal meter","Slower signal meter"};
    for (int i=0; i<6; ++i) {
        GtkWidget *button=gtk_button_new(), *icon=gtk_image_new_from_icon_name(icons[i],GTK_ICON_SIZE_MENU);
        gtk_image_set_pixel_size(GTK_IMAGE(icon),12);
        gtk_container_add(GTK_CONTAINER(button),icon);
        gtk_style_context_add_class(gtk_widget_get_style_context(button),"spectrum-arrow");
        atk_object_set_name(gtk_widget_get_accessible(button),names[i]);
        g_object_set_data(G_OBJECT(button),"meter-index",GINT_TO_POINTER(i));
        g_signal_connect(button,"clicked",G_CALLBACK(signal_meter_clicked),m);
        m->buttons[i]=button; gtk_overlay_add_overlay(GTK_OVERLAY(overlay),button);
    }
    g_signal_connect(overlay,"get-child-position",G_CALLBACK(signal_meter_position),m);
    signal_meter_refresh(m);
    return overlay;
}

static void signal_meter_save(SignalMeter *m, GKeyFile *config) {
    g_key_file_set_double(config,"display","meter_reference_dbfs",m->reference);
    g_key_file_set_double(config,"display","meter_range_db",m->span);
    g_key_file_set_double(config,"display","meter_seconds_per_point",m->interval);
}
#endif
