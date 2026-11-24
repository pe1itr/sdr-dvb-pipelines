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
typedef struct { double x,y,w,h; } PageRect;
static PageRect page_rect(GtkWidget *area) {
    double width=gtk_widget_get_allocated_width(area), height=gtk_widget_get_allocated_height(area);
    double w=MIN(width,height*4.0/3.0), h=w*3.0/4.0;
    return (PageRect){(width-w)/2,(height-h)/2,w,h};
}
static unsigned page_link_at(TsInfoView *v, double x, double y) {
    PageRect r=page_rect(v->page);
    if (!v->snapshot.page || x<r.x || y<r.y+r.h*24/25 || x>=r.x+r.w || y>=r.y+r.h) return 0;
    return v->snapshot.footer_links[(int)((x-r.x)*40/r.w)];
}
static void color_clicked(GtkButton *button, gpointer data) {
    TsInfoView *v=data;
    unsigned i=GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button),"color-index"));
    unsigned page=v->snapshot.color_pages[i];
    if (page) gtk_spin_button_set_value(GTK_SPIN_BUTTON(v->page_number),page);
}
static gboolean click_page(GtkWidget *area, GdkEventButton *event, gpointer data) {
    (void)area;
    TsInfoView *v=data;
    unsigned number=page_link_at(v,event->x,event->y);
    if (event->button!=1 || !number) return FALSE;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(v->page_number),number);
    return TRUE;
}
static gboolean page_tooltip(GtkWidget *area, gint x, gint y, gboolean keyboard,
                             GtkTooltip *tooltip, gpointer data) {
    (void)area;
    unsigned number=keyboard ? 0 : page_link_at(data,x,y);
    if (!number) return FALSE;
    char text[32];
    g_snprintf(text,sizeof(text),"Open pagina %u",number);
    gtk_tooltip_set_text(tooltip,text);
    return TRUE;
}
static gboolean draw_page(GtkWidget *area, cairo_t *cr, gpointer data) {
    TsInfoView *v=data; TsInfoSnapshot *s=&v->snapshot;
    PageRect r=page_rect(area);
    cairo_set_source_rgb(cr,0,0,0); cairo_paint(cr);
    if (!s->page || s->width<=0 || s->height<=0) {
        if (area==v->hdmi_page) {
            char message[64];
            g_snprintf(message,sizeof(message),"Wachten op teletekstpagina %u",s->page_number ? s->page_number : 100);
            PangoLayout *layout=gtk_widget_create_pango_layout(area,message);
            PangoFontDescription *font=pango_font_description_from_string("Sans Bold");
            pango_font_description_set_absolute_size(font,MAX(14,r.h/24)*PANGO_SCALE);
            pango_layout_set_font_description(layout,font);
            int w,h;
            pango_layout_get_pixel_size(layout,&w,&h);
            cairo_set_source_rgb(cr,1,1,1);
            cairo_move_to(cr,r.x+(r.w-w)/2,r.y+(r.h-h)/2);
            pango_cairo_show_layout(cr,layout);
            pango_font_description_free(font);
            g_object_unref(layout);
        }
        return FALSE;
    }
    GBytes *frame=s->page;
    gboolean reveal=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(v->reveal));
    if (reveal) frame=v->flash_on ? s->page_reveal : s->page_reveal_flash_off;
    else if (!v->flash_on) frame=s->page_flash_off;
    if (!frame) frame=s->page;
    /* Keep a TV-shaped page even if a layout allocates a different aspect.
     * UI controls never consume rows or alter character proportions. */
    cairo_surface_t *surface=cairo_image_surface_create_for_data(
        (unsigned char *)g_bytes_get_data(frame,NULL),CAIRO_FORMAT_RGB24,s->width,s->height,s->width*4);
    cairo_save(cr);
    cairo_translate(cr,r.x,r.y);
    cairo_scale(cr,r.w/s->width,r.h/s->height); cairo_set_source_surface(cr,surface,0,0);
    cairo_pattern_set_filter(cairo_get_source(cr),CAIRO_FILTER_NEAREST);
    cairo_paint(cr); cairo_restore(cr); cairo_surface_destroy(surface);
    return FALSE;
}
GtkWidget *ts_info_view_create_hdmi_page(TsInfoView *v) {
    v->hdmi_page=gtk_drawing_area_new();
    g_object_add_weak_pointer(G_OBJECT(v->hdmi_page),(gpointer *)&v->hdmi_page);
    gtk_widget_set_no_show_all(v->hdmi_page,TRUE);
    g_signal_connect(v->hdmi_page,"draw",G_CALLBACK(draw_page),v);
    return v->hdmi_page;
}

static void update(TsInfoView *v, TsInfoSnapshot *s, gboolean force) {
    unsigned page=s->page_number ? s->page_number : 100;
    v->updating=TRUE;
    /* Do not refresh an unchanged value: that would overwrite digits while
     * the user is typing, before Enter/focus-out commits the spin entry. */
    if (gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(v->page_number))!=(int)page)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(v->page_number),page);
    v->updating=FALSE;
    gtk_widget_set_sensitive(v->page_controls,v->backend!=NULL);
    for (unsigned i=0;i<4;i++)
        gtk_widget_set_visible(v->color_buttons[i],s->color_pages[i]!=0);
    char *page_name=g_strdup_printf("Teletext page %u",page);
    atk_object_set_name(gtk_widget_get_accessible(v->page),page_name);
    gboolean catalog=force || s->count!=v->snapshot.count || s->selected!=v->snapshot.selected ||
        memcmp(s->services,v->snapshot.services,sizeof(s->services));
    if (catalog) {
        v->updating=TRUE;
        for (unsigned window=0;window<G_N_ELEMENTS(v->dialogs);window++) {
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
    }
    if (force || memcmp(s->events,v->snapshot.events,sizeof(s->events))) {
        char *text=event_text(s);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(v->eit)),text,-1); g_free(text);
    }
    if (force || catalog || s->pmt_section!=v->snapshot.pmt_section) {
        char *text=ts_info_pmt_text(s);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(v->pmt)),text,-1);
        g_free(text);
    }
    gboolean ready[2]={s->events[0].valid || s->events[1].valid,s->teletext_announced};
    for (unsigned i=0;i<2;i++) {
        char *status;
        if (i==1 && !s->teletext_decoder && v->backend)
            status=g_strdup("Teletext decoder unavailable (libzvbi initialization failed)");
        else if (i==1 && !s->page)
            status=s->teletext_announced ? g_strdup_printf("Waiting for Teletext page %u",page) :
                g_strdup(s->pmt_seen ? "No Teletext announced for this service" : "Waiting for service information");
        else status=received_text(i ? s->teletext_received : s->eit_received);
        if (!i && s->eit_received && !ready[i]) { g_free(status); status=g_strdup("EIT received without current/next events"); }
        GtkStyleContext *style=gtk_widget_get_style_context(v->buttons[i]);
        if (ready[i]) gtk_style_context_add_class(style,"information-ready");
        else gtk_style_context_remove_class(style,"information-ready");
        char *tip=g_strdup_printf("%s%s\n%s",i ? "Teletext" : "EIT",ready[i] ? " available" : "",status);
        gtk_widget_set_tooltip_text(v->buttons[i],tip); g_free(tip);
        gtk_label_set_text(GTK_LABEL(v->status[i]),status); g_free(status);
        atk_object_set_description(gtk_widget_get_accessible(v->buttons[i]),ready[i] ? "Information available" : "Waiting for information");
    }
    g_free(page_name);
    gboolean image_changed=force || s->page!=v->snapshot.page || s->page_number!=v->snapshot.page_number;
    if (force || s->selected!=v->snapshot.selected || !s->page)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->reveal),FALSE);
    ts_info_snapshot_clear(&v->snapshot); v->snapshot=*s; memset(s,0,sizeof(*s));
    if (image_changed) {
        gtk_widget_queue_draw(v->page);
        if (v->hdmi_page) gtk_widget_queue_draw(v->hdmi_page);
    }
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
static void page_changed(GtkSpinButton *spin, gpointer data) {
    TsInfoView *v=data;
    if (v->updating || !v->backend) return;
    ts_info_backend_set_page(v->backend,gtk_spin_button_get_value_as_int(spin));
    v->next_tick=0; ts_info_view_tick(v,v->backend);
}
static void page_step(GtkButton *button, gpointer data) {
    TsInfoView *v=data;
    int step=GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button),"page-step"));
    gtk_spin_button_update(GTK_SPIN_BUTTON(v->page_number));
    int page=step ? gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(v->page_number))+step : 100;
    if (page<100) page=899;
    if (page>899) page=100;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(v->page_number),page);
}
static void hide_dialog(GtkDialog *dialog, gint response, gpointer data) {
    (void)response; (void)data; gtk_widget_hide(GTK_WIDGET(dialog));
}
static void open_information(GtkButton *button, gpointer data) {
    TsInfoView *v=data;
    v->next_tick=0; ts_info_view_tick(v,v->backend);
    unsigned index=GTK_WIDGET(button)==v->buttons[2] ? 2 : GTK_WIDGET(button)==v->buttons[1] ? 1 : 0;
    GtkWidget *dialog=v->dialogs[index];
    gtk_widget_show_all(dialog);
    gtk_window_present(GTK_WINDOW(dialog));
}
/* Conventional slow Teletext flash: one second, 3/4 on and 1/4 off.
 * Use monotonic time so system-clock adjustments cannot stop the animation. */
static gboolean flash_tick(gpointer data) {
    TsInfoView *v=data;
    gboolean on=(g_get_monotonic_time()%G_USEC_PER_SEC)<750000;
    if (on!=v->flash_on) {
        v->flash_on=on;
        if (v->snapshot.page && v->hdmi_page && gtk_widget_get_visible(v->hdmi_page)) gtk_widget_queue_draw(v->hdmi_page);
        if (v->snapshot.page && gtk_widget_get_visible(v->dialogs[1])) gtk_widget_queue_draw(v->page);
    }
    return G_SOURCE_CONTINUE;
}
void ts_info_view_create(TsInfoView *v, GtkWindow *parent, GtkWidget *bar, GtkWidget *pmt_item) {
    const char *titles[]={"EIT","Teletext","PMT"};
    const char *names[]={"eit-information","teletext-information","pmt-information"};
    for (unsigned i=0;i<G_N_ELEMENTS(v->dialogs);i++) {
        v->dialogs[i]=gtk_dialog_new_with_buttons(titles[i],parent,GTK_DIALOG_DESTROY_WITH_PARENT,
            "Close",GTK_RESPONSE_CLOSE,NULL);
        gtk_widget_set_name(v->dialogs[i],names[i]);
        gtk_window_set_default_size(GTK_WINDOW(v->dialogs[i]),680,610);
        g_signal_connect(v->dialogs[i],"response",G_CALLBACK(hide_dialog),v);
        g_signal_connect(v->dialogs[i],"delete-event",G_CALLBACK(gtk_widget_hide_on_delete),NULL);
        GtkWidget *content=gtk_dialog_get_content_area(GTK_DIALOG(v->dialogs[i]));
        gtk_container_set_border_width(GTK_CONTAINER(content),8); gtk_box_set_spacing(GTK_BOX(content),8);
        GtkWidget *row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
        gtk_box_pack_start(GTK_BOX(row),gtk_label_new("Information for"),FALSE,FALSE,0);
        v->services[i]=gtk_combo_box_text_new(); gtk_box_pack_start(GTK_BOX(row),v->services[i],TRUE,TRUE,0);
        gtk_widget_set_tooltip_text(v->services[i],"Select the service for the information windows; this does not change the player.");
        gtk_box_pack_start(GTK_BOX(content),row,FALSE,FALSE,0);
        g_signal_connect(v->services[i],"changed",G_CALLBACK(service_changed),v);
        if (i==2) v->buttons[i]=pmt_item;
        else {
            v->buttons[i]=gtk_button_new_with_label(titles[i]);
            gtk_style_context_add_class(gtk_widget_get_style_context(v->buttons[i]),"information-button");
            gtk_box_pack_start(GTK_BOX(bar),v->buttons[i],FALSE,FALSE,0);
        }
        g_signal_connect(v->buttons[i],"clicked",G_CALLBACK(open_information),v);
        GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);
        gtk_box_pack_start(GTK_BOX(content),box,TRUE,TRUE,0);
        if (i!=2) {
            v->status[i]=gtk_label_new(""); gtk_label_set_xalign(GTK_LABEL(v->status[i]),0);
        }
        if (i!=1) {
            if (!i) {
                gtk_label_set_line_wrap(GTK_LABEL(v->status[i]),TRUE);
                gtk_box_pack_start(GTK_BOX(box),v->status[i],FALSE,FALSE,0);
            }
            GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL);
            gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
            GtkWidget *view=gtk_text_view_new();
            if (i==2) v->pmt=view; else v->eit=view;
            gtk_text_view_set_editable(GTK_TEXT_VIEW(view),FALSE);
            gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view),FALSE);
            gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view),GTK_WRAP_WORD_CHAR);
            gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view),8); gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view),8);
            gtk_container_add(GTK_CONTAINER(scroll),view); gtk_box_pack_start(GTK_BOX(box),scroll,TRUE,TRUE,0);
        } else {
            v->page_controls=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
            gtk_box_pack_start(GTK_BOX(v->page_controls),gtk_label_new("Pagina"),FALSE,FALSE,0);
            v->page_number=gtk_spin_button_new_with_range(100,899,1);
            gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(v->page_number),TRUE);
            gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(v->page_number),TRUE);
            gtk_entry_set_width_chars(GTK_ENTRY(v->page_number),3);
            gtk_widget_set_tooltip_text(v->page_number,"Typ een pagina (100–899) en druk op Enter. Eerder ontvangen pagina’s openen direct.");
            atk_object_set_name(gtk_widget_get_accessible(v->page_number),"Teletekstpagina");
            gtk_box_pack_start(GTK_BOX(v->page_controls),v->page_number,FALSE,FALSE,0);
            g_signal_connect(v->page_number,"value-changed",G_CALLBACK(page_changed),v);
            const char *labels[]={"Vorige","Volgende","Index (100)"};
            const int steps[]={-1,1,0};
            for (unsigned j=0;j<G_N_ELEMENTS(labels);j++) {
                GtkWidget *button=gtk_button_new_with_label(labels[j]);
                g_object_set_data(G_OBJECT(button),"page-step",GINT_TO_POINTER(steps[j]));
                g_signal_connect(button,"clicked",G_CALLBACK(page_step),v);
                gtk_box_pack_start(GTK_BOX(v->page_controls),button,FALSE,FALSE,0);
            }
            GtkWidget *aspect=gtk_aspect_frame_new(NULL,.5,.5,4.0/3.0,FALSE);
            gtk_frame_set_shadow_type(GTK_FRAME(aspect),GTK_SHADOW_NONE);
            gtk_box_pack_start(GTK_BOX(box),aspect,TRUE,TRUE,0);
            v->page=gtk_drawing_area_new();
            gtk_widget_set_name(v->page,"teletext-screen");
            gtk_widget_set_size_request(v->page,320,240);
            gtk_container_add(GTK_CONTAINER(aspect),v->page);
            gtk_widget_add_events(v->page,GDK_BUTTON_PRESS_MASK);
            g_signal_connect(v->page,"button-press-event",G_CALLBACK(click_page),v);
            gtk_widget_set_has_tooltip(v->page,TRUE);
            g_signal_connect(v->page,"query-tooltip",G_CALLBACK(page_tooltip),v);
            gtk_box_pack_start(GTK_BOX(box),v->page_controls,FALSE,FALSE,0);
            GtkWidget *colors=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
            gtk_box_pack_start(GTK_BOX(box),colors,FALSE,FALSE,0);
            const char *names[]={"Rood","Groen","Geel","Blauw"};
            const char *inks[]={"#e53935","#21a344","#e6b800","#3978da"};
            for (unsigned j=0;j<4;j++) {
                GtkWidget *button=gtk_button_new();
                v->color_buttons[j]=button;
                GtkWidget *label=gtk_label_new(NULL);
                char *markup=g_strdup_printf("<span foreground='%s'>●</span> %s",inks[j],names[j]);
                gtk_label_set_markup(GTK_LABEL(label),markup);
                g_free(markup);
                gtk_container_add(GTK_CONTAINER(button),label);
                gtk_widget_show(label);
                gtk_widget_set_no_show_all(button,TRUE);
                gtk_box_pack_start(GTK_BOX(colors),button,TRUE,TRUE,0);
                g_object_set_data(G_OBJECT(button),"color-index",GUINT_TO_POINTER(j));
                g_signal_connect(button,"clicked",G_CALLBACK(color_clicked),v);
            }
            GtkWidget *controls=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
            gtk_box_pack_start(GTK_BOX(box),controls,FALSE,FALSE,0);
            gtk_label_set_ellipsize(GTK_LABEL(v->status[i]),PANGO_ELLIPSIZE_END);
            gtk_box_pack_start(GTK_BOX(controls),v->status[i],TRUE,TRUE,0);
            v->reveal=gtk_check_button_new_with_label("Reveal");
            gtk_widget_set_tooltip_text(v->reveal,"Show concealed Teletext text");
            gtk_box_pack_end(GTK_BOX(controls),v->reveal,FALSE,FALSE,0);
            g_signal_connect_swapped(v->reveal,"toggled",G_CALLBACK(gtk_widget_queue_draw),v->page);
            g_signal_connect(v->page,"draw",G_CALLBACK(draw_page),v);
            atk_object_set_name(gtk_widget_get_accessible(v->page),"Teletext page 100");
        }
    }
    v->flash_on=TRUE;
    v->flash_timer=g_timeout_add(50,flash_tick,v);
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
    if (v->hdmi_page) gtk_widget_hide(v->hdmi_page);
    v->backend=NULL; v->next_tick=0;
    if (!v->dialogs[0]) return;
    TsInfoSnapshot s={0}; update(v,&s,TRUE);
}
void ts_info_view_destroy(TsInfoView *v) {
    if (v->flash_timer) { g_source_remove(v->flash_timer); v->flash_timer=0; }
    ts_info_snapshot_clear(&v->snapshot); v->backend=NULL;
    for (unsigned i=0;i<G_N_ELEMENTS(v->dialogs);i++) {
        if (v->dialogs[i]) gtk_widget_destroy(v->dialogs[i]);
        v->dialogs[i]=NULL;
    }
}
