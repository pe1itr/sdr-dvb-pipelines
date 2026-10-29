#ifndef LINRAD_SCATTER_VIEW_H
#define LINRAD_SCATTER_VIEW_H
#include <gtk/gtk.h>
#include "scatter_backend.h"
typedef struct {
    GtkWidget *frame, *area, *progress, *phase, *service, *provider, *details;
    GdkPixbuf *image;
    GBytes *pixels;
} ScatterView;
GtkWidget *scatter_view_create(ScatterView *v);
void scatter_view_tick(ScatterView *v, ScatterBackend *s);
void scatter_view_reset(ScatterView *v);
#endif
