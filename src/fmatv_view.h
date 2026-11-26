#ifndef FMATV_VIEW_H
#define FMATV_VIEW_H
#include "../components/fmatv/fmatv.h"
#include "video_view.h"
typedef struct {
    GtkWidget *stack, *lamps[9], *values[4], *caption;
    gint64 caption_second;
} FmatvService;
void fmatv_service_create(FmatvService *service, GtkWidget *frame);
void fmatv_service_update(FmatvService *service, unsigned enabled, unsigned detected,
                          const FmatvFieldInfo *field, guint64 count, unsigned stacked);
gboolean fmatv_view_start(VideoView *v, int fd, GError **error);
#endif
