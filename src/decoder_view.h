#ifndef DECODER_VIEW_H
#define DECODER_VIEW_H
#include <gtk/gtk.h>
#include "decoder_monitor.h"
#define DECODER_HISTORY_FRAMES 12
#define DECODER_HISTORY_AGE (G_USEC_PER_SEC / 2)
typedef struct {
    GtkWidget *area, *service_frame, *state, *lamps[4], *service, *provider;
    GtkWidget *snr, *pilot, *cfo, *rms, *peak, *buffer, *margin;
    GtkWidget *modulation, *fec, *guard, *packets, *written, *rs_ok, *rs_errors, *cc_errors;
    GtkWidget *video_codec, *audio_codec;
    int fd, port;
    gint64 status_time, points_time, buffer_time;
    gboolean active;
    DecoderStatus status;
    char selected_service[128], selected_provider[128];
    char selected_video_codec[32], selected_audio_codec[32];
    gboolean service_valid;
    DecoderConstellation points;
    DecoderConstellation history[DECODER_HISTORY_FRAMES];
    gint64 history_time[DECODER_HISTORY_FRAMES];
    unsigned history_next;
    DecoderBuffer queue;
} DecoderView;
GtkWidget *decoder_view_create(DecoderView *v);
gboolean decoder_view_start(DecoderView *v);
void decoder_view_stop(DecoderView *v);
void decoder_view_tick(DecoderView *v, gboolean active, const char *mode);
#endif
