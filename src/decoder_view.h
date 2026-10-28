#ifndef DECODER_VIEW_H
#define DECODER_VIEW_H
#include <gtk/gtk.h>
#include "decoder_monitor.h"
typedef struct {
    GtkWidget *area, *state, *lamps[4], *service, *provider;
    GtkWidget *snr, *pilot, *cfo, *rms, *peak, *buffer;
    GtkWidget *modulation, *fec, *guard, *packets, *written, *rs_ok, *rs_errors, *cc_errors;
    int fd, port;
    gint64 status_time, points_time, buffer_time;
    gboolean active;
    DecoderStatus status;
    DecoderConstellation points;
    DecoderBuffer queue;
} DecoderView;
GtkWidget *decoder_view_create(DecoderView *v);
gboolean decoder_view_start(DecoderView *v);
void decoder_view_stop(DecoderView *v);
void decoder_view_tick(DecoderView *v, gboolean active, const char *mode);
#endif
