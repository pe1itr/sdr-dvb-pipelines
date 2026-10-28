#ifndef IQ_VIEW_H
#define IQ_VIEW_H
#include "iq_monitor.h"
#include <math.h>
#include <sys/socket.h>
#define IQ_VIEW_ROWS 180
typedef struct {
    GtkWidget *area, *floor_controls, *span_controls, *level_buttons[4], *filter_toggle, *filter_controls;
    int fd, port, rows;
    guint timer;
    IQMonitorFrame frame;
    float history[IQ_VIEW_ROWS][IQ_MONITOR_BINS];
    double bandwidth, center, rate, floor, span;
    gboolean enabled, active;
    gint64 last_frame;
} IQView;
static gboolean iq_view_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    IQView *v = data;
    double w=gtk_widget_get_allocated_width(widget), h=gtk_widget_get_allocated_height(widget);
    double left=65, right=w-40, wf=5, wh=h/2-32;
    double top=h/2+25, bottom=h-30;
    double floor=v->floor, span=v->span;
    cairo_set_source_rgb(cr, 0,0,0); cairo_paint(cr);
    cairo_select_font_face(cr,"sans",CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL); cairo_set_font_size(cr,12);
    cairo_set_source_rgb(cr,.9,.9,.9);
    char text[200];
    if (right <= left || wh <= 0) return FALSE;
    for (int j=0;j<=4;++j) {
        double y=top+(bottom-top)*j/4;
        cairo_set_source_rgb(cr,.2,.2,.2); cairo_move_to(cr,left,y); cairo_line_to(cr,right,y); cairo_stroke(cr);
        g_snprintf(text,sizeof(text),"%.0f",floor+span-span*j/4);
        cairo_set_source_rgb(cr,.8,.8,.8); cairo_move_to(cr,12,y+4); cairo_show_text(cr,text);
    }
    cairo_move_to(cr,4,top-12); cairo_show_text(cr,"dBFS/Hz");
    if (v->rows) {
        cairo_surface_t *surface=cairo_image_surface_create(CAIRO_FORMAT_RGB24,IQ_MONITOR_BINS,IQ_VIEW_ROWS);
        unsigned char *bytes=cairo_image_surface_get_data(surface);
        int stride=cairo_image_surface_get_stride(surface);
        for(int y=0;y<IQ_VIEW_ROWS;++y) for(int x=0;x<IQ_MONITOR_BINS;++x) {
            double t=y<v->rows ? CLAMP((v->history[y][x]-floor)/span,0,1) : 0;
            unsigned r=255*CLAMP(3*t-1.5,0,1), g=255*CLAMP(3*t-.5,0,1), b=255*CLAMP(3*t,0,1)*(1-CLAMP(3*t-2,0,1));
            ((uint32_t *)(bytes+y*stride))[x]=(r<<16)|(g<<8)|b;
        }
        cairo_surface_mark_dirty(surface); cairo_save(cr); cairo_translate(cr,left,wf);
        cairo_scale(cr,(right-left)/IQ_MONITOR_BINS,wh/IQ_VIEW_ROWS);
        cairo_set_source_surface(cr,surface,0,0); cairo_paint(cr); cairo_restore(cr); cairo_surface_destroy(surface);
        cairo_set_source_rgb(cr,.2,1,.45);
        for(int i=0;i<IQ_MONITOR_BINS;++i) {
            double x=left+(right-left)*i/(IQ_MONITOR_BINS-1), y=bottom-CLAMP((v->frame.power[i]-floor)/span,0,1)*(bottom-top);
            if(i) cairo_line_to(cr,x,y); else cairo_move_to(cr,x,y);
        }
        cairo_stroke(cr);

    }
    double rate=v->rate;
    if(rate>0) {
        for(int j=0;j<=4;++j) {
            double x=left+(right-left)*j/4;
            g_snprintf(text,sizeof(text),"%.6f",(v->center+(j/4.0-.5)*rate)/1e6);
            cairo_text_extents_t ext; cairo_text_extents(cr,text,&ext);
            double tx=CLAMP(x-ext.width/2,left,right-ext.width);
            cairo_set_source_rgb(cr,.9,.9,.9);
            cairo_move_to(cr,tx,wf+wh+18); cairo_show_text(cr,text);
            cairo_move_to(cr,tx,bottom+18); cairo_show_text(cr,text);
        }
        double half=CLAMP(v->bandwidth/rate,0,1)*(right-left)/2, center=(left+right)/2;
        cairo_set_source_rgba(cr,0.2,.7,1,v->enabled?.12:.035);
        cairo_rectangle(cr,center-half,top,2*half,bottom-top); cairo_fill(cr);
        cairo_set_source_rgba(cr,.3,.8,1,v->enabled?.9:.35);
        for(int side=-1;side<=1;side+=2) {
            cairo_move_to(cr,center+side*half,top); cairo_line_to(cr,center+side*half,bottom);
            cairo_move_to(cr,center+side*half,wf); cairo_line_to(cr,center+side*half,wf+wh);
        }
        cairo_stroke(cr);
        /* Right-align the bandwidth just inside the right filter boundary;
         * recompute its position on every draw so it follows the moving line. */
        g_snprintf(text,sizeof(text),"BW %.0f Hz%s",v->bandwidth,v->enabled ? "" : " (uit)");
        cairo_text_extents_t ext; cairo_text_extents(cr,text,&ext);
        double tx=center+half-6-ext.width-ext.x_bearing, ty=top+18;
        cairo_set_source_rgba(cr,0,0,0,.85);
        cairo_rectangle(cr,tx+ext.x_bearing-3,ty+ext.y_bearing-3,ext.width+6,ext.height+6);
        cairo_fill(cr);
        cairo_set_source_rgb(cr,v->enabled ? .3 : .65,v->enabled ? .8 : .65,v->enabled ? 1 : .65);
        cairo_move_to(cr,tx,ty); cairo_show_text(cr,text);
    }
    cairo_set_source_rgb(cr,.8,.8,.8);
    cairo_move_to(cr,left,top-10); cairo_show_text(cr,"Spectrum · ontvangstfrequentie (MHz)");
    return FALSE;
}
static void iq_view_receive(IQView *v) {
    IQMonitorFrame frame; int count=0;
    while(v->fd>=0 && count++<32) {
        ssize_t n=recv(v->fd,&frame,sizeof(frame),MSG_DONTWAIT|MSG_TRUNC);
        if(n<0) break;
        if(n!=sizeof(frame)||frame.magic!=IQ_MONITOR_MAGIC||!isfinite(frame.rate)||frame.rate<=0) continue;
        gboolean valid=TRUE;
        for(int i=0;i<IQ_MONITOR_BINS;++i) if(!isfinite(frame.power[i])) valid=FALSE;
        if(!valid || !v->active) continue;
        if(frame.rate!=v->frame.rate) v->rows=0;
        memmove(v->history+1,v->history,(IQ_VIEW_ROWS-1)*sizeof(v->history[0]));
        memcpy(v->history[0],frame.power,sizeof(frame.power));
        v->frame=frame; v->rows=MIN(v->rows+1,IQ_VIEW_ROWS); v->last_frame=g_get_monotonic_time();
    }
    gtk_widget_queue_draw(v->area);
}

static void iq_view_level_refresh(IQView *v) {
    for (int i=0; i<4; ++i) {
        gboolean span=i>=2, up=i%2==0;
        double value=span ? v->span : v->floor;
        gtk_widget_set_sensitive(v->level_buttons[i], up ? value<(span ? 160 : -10) : value>(span ? 10 : -200));
        char *tip=g_strdup_printf("%s %s (5 dB) · nu %.0f %s",
            span ? "Bereik" : "Ondergrens", up ? "verhogen" : "verlagen", value, span ? "dB" : "dBFS/Hz");
        gtk_widget_set_tooltip_text(v->level_buttons[i],tip); g_free(tip);
    }
    gtk_widget_queue_draw(v->area);
}

static void iq_view_level_clicked(GtkButton *button, gpointer data) {
    IQView *v=data;
    int index=GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button),"level-index"));
    double delta=index%2==0 ? 5 : -5;
    if (index>=2) v->span=CLAMP(v->span+delta,10,160);
    else v->floor=CLAMP(v->floor+delta,-200,-10);
    iq_view_level_refresh(v);
}

/* Anchor the controls to the spectrum axes, including after resizing. */
static gboolean iq_view_control_position(GtkOverlay *overlay, GtkWidget *child, GdkRectangle *rect, gpointer data) {
    (void)overlay; IQView *v=data;
    if (child!=v->floor_controls && child!=v->span_controls && child!=v->filter_controls) return FALSE;
    GtkRequisition size;
    gtk_widget_get_preferred_size(child,NULL,&size);
    int w=gtk_widget_get_allocated_width(v->area), h=gtk_widget_get_allocated_height(v->area);
    *rect=(GdkRectangle){child==v->filter_controls ? 69 : w-size.width-5,
                         child==v->floor_controls ? h-33-size.height : h/2+28,
                         size.width, size.height};
    return TRUE;
}

static double iq_view_saved_level(GKeyFile *config, const char *key, double fallback,
                                  double low, double high) {
    GError *error=NULL;
    double value=g_key_file_get_double(config,"display",key,&error);
    if (error || !isfinite(value) || value<low || value>high) value=fallback;
    g_clear_error(&error);
    return value;
}

static GtkWidget *iq_view_create(IQView *v, GKeyFile *config) {
    v->fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    socklen_t len=sizeof(addr);
    if(v->fd>=0 && (bind(v->fd,(struct sockaddr *)&addr,sizeof(addr)) || getsockname(v->fd,(struct sockaddr *)&addr,&len))) {close(v->fd);v->fd=-1;}
    if(v->fd>=0) v->port=ntohs(addr.sin_port);
    v->floor=iq_view_saved_level(config,"floor_dbfs_hz",-160,-200,-10);
    v->span=iq_view_saved_level(config,"range_db",100,10,160);
    v->area=gtk_drawing_area_new(); gtk_widget_set_size_request(v->area,600,280);
    gtk_widget_set_tooltip_text(v->area,v->fd<0 ? "IQ-monitor kon geen lokale socket openen" :
        "Waterval: nieuwste boven. IQ vóór het filter; blauw → groen → geel = meer vermogen. Frequentie in MHz.");
    g_signal_connect(v->area,"draw",G_CALLBACK(iq_view_draw),v);
    GtkWidget *overlay=gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(overlay),v->area);
    v->floor_controls=gtk_box_new(GTK_ORIENTATION_VERTICAL,1);
    v->span_controls=gtk_box_new(GTK_ORIENTATION_VERTICAL,1);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay),v->floor_controls);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay),v->span_controls);
    v->filter_toggle=gtk_toggle_button_new_with_label("BW");
    gtk_widget_set_name(v->filter_toggle,"spectrum-bw");
    atk_object_set_name(gtk_widget_get_accessible(v->filter_toggle),"IQ-bandfilter aan/uit");
    v->filter_controls=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,4);
    gtk_box_pack_start(GTK_BOX(v->filter_controls),v->filter_toggle,FALSE,FALSE,0);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay),v->filter_controls);
    for (int i=0; i<4; ++i) {
        GtkWidget *button=gtk_button_new(), *icon=gtk_image_new_from_icon_name(
            i%2==0 ? "pan-up-symbolic" : "pan-down-symbolic",GTK_ICON_SIZE_MENU);
        gtk_image_set_pixel_size(GTK_IMAGE(icon),12);
        gtk_container_add(GTK_CONTAINER(button),icon);
        gtk_style_context_add_class(gtk_widget_get_style_context(button),"spectrum-arrow");
        atk_object_set_name(gtk_widget_get_accessible(button),
            (const char *[]) {"Ondergrens verhogen", "Ondergrens verlagen", "Bereik verhogen", "Bereik verlagen"}[i]);
        g_object_set_data(G_OBJECT(button),"level-index",GINT_TO_POINTER(i));
        g_signal_connect(button,"clicked",G_CALLBACK(iq_view_level_clicked),v);
        gtk_box_pack_start(GTK_BOX(i<2 ? v->floor_controls : v->span_controls),button,FALSE,FALSE,0);
        v->level_buttons[i]=button;
    }
    g_signal_connect(overlay,"get-child-position",G_CALLBACK(iq_view_control_position),v);
    iq_view_level_refresh(v);
    return overlay;
}
#endif
