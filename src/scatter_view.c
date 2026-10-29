#include "scatter_view.h"

static gboolean draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    ScatterView *v = data;
    double w = gtk_widget_get_allocated_width(widget), h = gtk_widget_get_allocated_height(widget);
    cairo_set_source_rgb(cr, 0, 0, 0); cairo_paint(cr);
    if (v->image) {
        int iw = gdk_pixbuf_get_width(v->image), ih = gdk_pixbuf_get_height(v->image);
        double scale = MIN(w / iw, h / ih);
        cairo_translate(cr, (w - iw * scale) / 2, (h - ih * scale) / 2);
        cairo_scale(cr, scale, scale);
        gdk_cairo_set_source_pixbuf(cr, v->image, 0, 0); cairo_paint(cr);
    } else {
        PangoLayout *text = gtk_widget_create_pango_layout(widget, "Waiting for recovered image…");
        int tw, th; pango_layout_get_pixel_size(text, &tw, &th);
        cairo_set_source_rgb(cr, .65, .65, .65);
        cairo_move_to(cr, (w - tw) / 2, (h - th) / 2);
        pango_cairo_show_layout(cr, text); g_object_unref(text);
    }
    return FALSE;
}

static GtkWidget *label(GtkWidget *box) {
    GtkWidget *w = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(w), 0);
    gtk_label_set_ellipsize(GTK_LABEL(w), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(box), w, FALSE, FALSE, 0);
    return w;
}

GtkWidget *scatter_view_create(ScatterView *v) {
    v->frame = gtk_frame_new("TSscatterFix");
    gtk_widget_set_no_show_all(v->frame, TRUE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_container_add(GTK_CONTAINER(v->frame), box);
    v->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(v->area, 320, 160);
    gtk_widget_set_vexpand(v->area, TRUE);
    gtk_box_pack_start(GTK_BOX(box), v->area, TRUE, TRUE, 0);
    g_signal_connect(v->area, "draw", G_CALLBACK(draw), v);
    GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(info), 5);
    gtk_box_pack_start(GTK_BOX(box), info, FALSE, FALSE, 0);
    v->progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(v->progress), TRUE);
    gtk_widget_set_tooltip_text(v->progress, "Estimate based on received headers and usable picture data; not remaining time.");
    gtk_box_pack_start(GTK_BOX(info), v->progress, FALSE, FALSE, 0);
    v->phase = label(info); v->provider = label(info); v->service = label(info); v->details = label(info);
    scatter_view_reset(v);
    gtk_widget_show_all(box);
    return v->frame;
}

void scatter_view_reset(ScatterView *v) {
    g_clear_object(&v->image); g_clear_pointer(&v->pixels, g_bytes_unref);
    if (!v->frame) return;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(v->progress), 0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(v->progress), "0% · estimated progress");
    gtk_label_set_text(GTK_LABEL(v->phase), "Waiting for service");
    gtk_label_set_text(GTK_LABEL(v->provider), "Provider: —");
    gtk_label_set_text(GTK_LABEL(v->service), "Service: —");
    gtk_label_set_text(GTK_LABEL(v->details), "Headers: — · Keyframes: —");
    gtk_widget_queue_draw(v->area);
}

static void service_label(GtkWidget *label, const char *title, const char *value) {
    char *valid = g_utf8_make_valid(*value ? value : "—", -1);
    char *text = g_strdup_printf("%s: %s", title, valid);
    gtk_label_set_text(GTK_LABEL(label), text); gtk_widget_set_tooltip_text(label, text);
    g_free(text); g_free(valid);
}

void scatter_view_tick(ScatterView *v, ScatterBackend *s) {
    if (!v->frame || !s) return;
    g_mutex_lock(&s->mutex);
    SfRecoveryStatus status = s->status;
    guint64 dropped = s->dropped;
    GBytes *pixels = s->image ? g_bytes_ref(s->image) : NULL;
    int width = s->width, height = s->height, stride = s->stride;
    g_mutex_unlock(&s->mutex);
    if (pixels != v->pixels) {
        g_clear_object(&v->image); g_clear_pointer(&v->pixels, g_bytes_unref);
        v->pixels = pixels; pixels = NULL;
        if (v->pixels) v->image = gdk_pixbuf_new_from_bytes(v->pixels, GDK_COLORSPACE_RGB, FALSE, 8, width, height, stride);
        gtk_widget_queue_draw(v->area);
    }
    if (pixels) g_bytes_unref(pixels);
    static const char *phases[] = {"Waiting for service", "Collecting video headers", "Waiting for keyframe",
        "Ready to reconstruct", "Waiting for usable image", "Image recovered"};
    gtk_label_set_text(GTK_LABEL(v->phase), g_atomic_int_get(&s->recovery_failed) ?
        "Reconstruction failed; player continues" : phases[CLAMP(status.phase, 0, SF_IMAGE_READY)]);
    double progress = MIN(status.progress_percent, 100) / 100.0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(v->progress), progress);
    char text[256]; g_snprintf(text, sizeof(text), "%.0f%% · estimated progress", progress * 100);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(v->progress), text);
    service_label(v->provider, "Provider", status.provider_name);
    service_label(v->service, "Service", status.service_name);
    g_snprintf(text, sizeof(text), "Headers %u/%u · Keyframes %" G_GUINT64_FORMAT " · Images %" G_GUINT64_FORMAT,
        status.headers_found, status.headers_required, status.keyframes, status.decoded_images);
    gtk_label_set_text(GTK_LABEL(v->details), text);
    g_snprintf(text, sizeof(text), "Received: %" G_GUINT64_FORMAT " TS packets\nReconstruction attempts: %" G_GUINT64_FORMAT
        "\nSkipped on recovery branch: %" G_GUINT64_FORMAT " bytes", status.packets_received, status.decode_attempts, dropped);
    gtk_widget_set_tooltip_text(v->details, text);
}
