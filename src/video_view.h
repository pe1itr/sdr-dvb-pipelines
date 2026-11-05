#ifndef VIDEO_VIEW_H
#define VIDEO_VIEW_H
#include <gtk/gtk.h>
#include <vlc/vlc.h>

typedef struct VideoExternalChild VideoExternalChild;
typedef struct {
    GtkWidget *area, *container, *output_button, *notice;
    libvlc_instance_t *vlc;
    libvlc_media_player_t *player;
    GMutex mutex;
    GMutex output_mutex; /* Protects the relay's replaceable write descriptor. */
    unsigned char *pixels, *frame;
    unsigned width, height, pitch;
    double aspect;
    gboolean ready, dirty, local_display;
    gboolean active, muted;
    gboolean volume_initialized;
    GThread *relay;
    gint stopping, relay_failed;
    gint input_ended;
    guint output_generation;
    gboolean external;
    VideoExternalChild *external_child;
    GList *external_children; /* Also tracks terminating children until reaped. */
    char *gpu_program, *gpu_device;
    int source_fd, video_fd, udp_fd;
    guint timer;
    int fd;
    guint16 program;
    unsigned caching;
} VideoView;

GtkWidget *video_view_create(VideoView *v);
gboolean video_view_start(VideoView *v, int fd, const char *url,
                          const char *udp_destination, gboolean display, GError **error);
gboolean video_view_failed(VideoView *v);
void video_view_stop(VideoView *v);
void video_view_set_muted(VideoView *v, gboolean muted);
void video_view_set_gpu_player(VideoView *v, const char *program, const char *device);
gboolean video_view_set_external(VideoView *v, gboolean external, GError **error);
gboolean video_view_select_program(VideoView *v, guint16 program, GError **error);
void video_view_destroy(VideoView *v);
#endif
