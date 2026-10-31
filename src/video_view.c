#define _POSIX_C_SOURCE 200809L
#include "video_view.h"
#include <glib-unix.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Forward the original TS, including all services and aspect metadata. The
 * relay owns no GTK objects and all waits are interruptible during Stop. */
static gpointer relay_ts(gpointer data) {
    VideoView *v = data;
    unsigned char input[65536], packet[1316];
    size_t pending = 0;
    while (!g_atomic_int_get(&v->stopping)) {
        struct pollfd source = {.fd = v->source_fd, .events = POLLIN};
        int rc = poll(&source, 1, 100);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) goto failed;
        if (!rc) continue;
        ssize_t n = read(v->source_fd, input, sizeof(input));
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n < 0) goto failed;
        if (!n) break;
        if (v->udp_fd >= 0) {
            size_t at = 0;
            while (at < (size_t)n) {
                size_t copy = MIN(sizeof(packet) - pending, (size_t)n - at);
                memcpy(packet + pending, input + at, copy); pending += copy; at += copy;
                if (pending == sizeof(packet)) {
                    send(v->udp_fd, packet, pending, MSG_DONTWAIT); pending = 0;
                }
            }
            /* Do not delay sparse ScatterFix frames waiting for a full datagram. */
            size_t complete = pending / 188 * 188;
            if (complete) {
                send(v->udp_fd, packet, complete, MSG_DONTWAIT);
                pending -= complete; memmove(packet, packet + complete, pending);
            }
        }
        size_t at = 0;
        while (v->video_fd >= 0 && at < (size_t)n) {
            if (g_atomic_int_get(&v->stopping)) goto done;
            struct pollfd sink = {.fd = v->video_fd, .events = POLLOUT};
            rc = poll(&sink, 1, 100);
            if (rc < 0 && errno == EINTR) continue;
            if (rc < 0) goto failed;
            if (!rc) continue;
            ssize_t written = write(v->video_fd, input + at, (size_t)n - at);
            if (written < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (written <= 0) goto failed;
            at += written;
        }
    }
    goto done;
failed:
    g_atomic_int_set(&v->relay_failed, TRUE);
done:
    if (v->video_fd >= 0) { close(v->video_fd); v->video_fd = -1; }
    return NULL;
}

/* VLC callbacks run on decoder threads. Only the GTK timer touches widgets.
 * Frame dimensions never affect the widget's size request or allocation. */
static unsigned setup(void **opaque, char *chroma, unsigned *width, unsigned *height,
                      unsigned *pitches, unsigned *lines) {
    VideoView *v = *opaque;
    if (!*width || !*height || *width > 8192 || *height > 8192) return 0;
    unsigned pitch = (*width * 4 + 31) & ~31u;
    unsigned rows = (*height + 31) & ~31u;
    unsigned char *pixels = NULL, *frame = NULL;
    if (posix_memalign((void **)&pixels, 32, (size_t)pitch * rows) ||
        posix_memalign((void **)&frame, 32, (size_t)pitch * rows)) {
        free(pixels); free(frame); return 0;
    }
    g_mutex_lock(&v->mutex);
    free(v->pixels); free(v->frame);
    v->pixels = pixels; v->frame = frame;
    v->width = *width; v->height = *height; v->pitch = pitch;
    v->ready = FALSE;
    g_mutex_unlock(&v->mutex);
    memcpy(chroma, "RV32", 4);
    *pitches = pitch; *lines = rows;
    return 1;
}

static void *lock_frame(void *opaque, void **planes) {
    VideoView *v = opaque;
    g_mutex_lock(&v->mutex);
    *planes = v->pixels;
    return NULL;
}

static void unlock_frame(void *opaque, void *picture, void *const *planes) {
    (void)picture; (void)planes;
    VideoView *v = opaque;
    g_mutex_unlock(&v->mutex);
}

static void display_frame(void *opaque, void *picture) {
    (void)picture;
    VideoView *v = opaque;
    g_mutex_lock(&v->mutex);
    memcpy(v->frame, v->pixels, (size_t)v->pitch * v->height);
    v->ready = v->dirty = TRUE;
    g_mutex_unlock(&v->mutex);
}

static gboolean draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    VideoView *v = data;
    double w = gtk_widget_get_allocated_width(widget), h = gtk_widget_get_allocated_height(widget);
    cairo_set_source_rgb(cr, 0, 0, 0); cairo_paint(cr);
    g_mutex_lock(&v->mutex);
    if (v->ready) {
        /* Broadcast SD often has non-square pixels (e.g. 720x576 for both
         * 4:3 and 16:9). Use the signalled display aspect, not pixel dimensions. */
        double aspect = v->aspect > 0 ? v->aspect : (double)v->width / v->height;
        double dw = MIN(w, h * aspect), dh = dw / aspect;
        cairo_surface_t *surface = cairo_image_surface_create_for_data(v->frame,
            CAIRO_FORMAT_RGB24, v->width, v->height, v->pitch);
        cairo_translate(cr, (w - dw) / 2, (h - dh) / 2);
        cairo_scale(cr, dw / v->width, dh / v->height);
        cairo_set_source_surface(cr, surface, 0, 0);
        cairo_paint(cr);
        cairo_surface_destroy(surface);
    } else {
        PangoLayout *text = gtk_widget_create_pango_layout(widget,
            v->active ? (v->local_display ? "Waiting for video…" : "Streaming via UDP") : "Received video");
        int tw, th; pango_layout_get_pixel_size(text, &tw, &th);
        cairo_set_source_rgb(cr, .65, .65, .65);
        cairo_move_to(cr, (w - tw) / 2, (h - th) / 2);
        pango_cairo_show_layout(cr, text); g_object_unref(text);
    }
    g_mutex_unlock(&v->mutex);
    return FALSE;
}

static gboolean tick(gpointer data) {
    VideoView *v = data;
    if (v->player) {
        libvlc_media_t *media = libvlc_media_player_get_media(v->player);
        if (media) {
            libvlc_media_track_t **tracks = NULL;
            unsigned count = libvlc_media_tracks_get(media, &tracks);
            int selected = libvlc_video_get_track(v->player);
            for (unsigned i = 0; i < count; ++i) {
                /* Before track selection completes, use the first video. */
                if (tracks[i]->i_type != libvlc_track_video || (selected >= 0 && tracks[i]->i_id != selected)) continue;
                libvlc_video_track_t *video = tracks[i]->video;
                if (!video->i_width || !video->i_height) continue;
                double aspect = (double)video->i_width / video->i_height;
                if (video->i_sar_num && video->i_sar_den) aspect *= (double)video->i_sar_num / video->i_sar_den;
                if (video->i_orientation >= libvlc_video_orient_left_top) aspect = 1 / aspect;
                g_mutex_lock(&v->mutex);
                if (v->aspect != aspect) { v->aspect = aspect; v->dirty = TRUE; }
                g_mutex_unlock(&v->mutex);
                break;
            }
            libvlc_media_tracks_release(tracks, count);
            libvlc_media_release(media);
        }
    }
    g_mutex_lock(&v->mutex);
    gboolean dirty = v->dirty; v->dirty = FALSE;
    g_mutex_unlock(&v->mutex);
    if (dirty) gtk_widget_queue_draw(v->area);
    return G_SOURCE_CONTINUE;
}

GtkWidget *video_view_create(VideoView *v) {
    g_mutex_init(&v->mutex); v->fd = v->source_fd = v->video_fd = v->udp_fd = -1;
    v->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(v->area, 320, 180);
    gtk_widget_set_hexpand(v->area, TRUE); gtk_widget_set_vexpand(v->area, TRUE);
    g_signal_connect(v->area, "draw", G_CALLBACK(draw), v);
    v->timer = g_timeout_add(33, tick, v);
    v->container = gtk_frame_new("Received video");
    gtk_container_add(GTK_CONTAINER(v->container), v->area);
    return v->container;
}

void video_view_set_muted(VideoView *v, gboolean muted) {
    v->muted = muted;
    if (v->player) libvlc_audio_set_mute(v->player, muted);
    if (v->container) gtk_frame_set_label(GTK_FRAME(v->container),
        muted ? "Received video · Muted (M)" : "Received video");
}

void video_view_stop(VideoView *v) {
    g_atomic_int_set(&v->stopping, TRUE);
    if (v->relay) { g_thread_join(v->relay); v->relay = NULL; }
    if (v->video_fd >= 0) { close(v->video_fd); v->video_fd = -1; }
    if (v->player) {
        libvlc_media_player_stop(v->player);
        libvlc_media_player_release(v->player); v->player = NULL;
    }
    if (v->vlc) { libvlc_release(v->vlc); v->vlc = NULL; }
    if (v->fd >= 0) { close(v->fd); v->fd = -1; }
    if (v->source_fd >= 0) { close(v->source_fd); v->source_fd = -1; }
    if (v->udp_fd >= 0) { close(v->udp_fd); v->udp_fd = -1; }
    v->active = FALSE;
    g_mutex_lock(&v->mutex);
    free(v->pixels); free(v->frame); v->pixels = v->frame = NULL;
    v->ready = FALSE; v->dirty = TRUE; v->aspect = 0;
    g_mutex_unlock(&v->mutex);
}

gboolean video_view_start(VideoView *v, int fd, const char *url,
                          const char *udp_destination, gboolean display, GError **error) {
    video_view_stop(v);
    v->local_display = display;
    g_atomic_int_set(&v->stopping, FALSE); g_atomic_int_set(&v->relay_failed, FALSE);
    if (url) {
        char host[16]; unsigned port;
        struct sockaddr_in addr = {.sin_family = AF_INET};
        if (sscanf(url, "udp://@%15[^:]:%u", host, &port) != 2 || port < 1 || port > 65535 ||
            inet_pton(AF_INET, host, &addr.sin_addr) != 1) goto fail;
        addr.sin_port = htons(port);
        v->source_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (v->source_fd < 0 || bind(v->source_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto fail;
    } else v->source_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (v->source_fd < 0) goto fail;
    if (udp_destination && *udp_destination) {
        char host[16]; unsigned port;
        struct sockaddr_in addr = {.sin_family = AF_INET};
        if (sscanf(udp_destination, "%15[^:]:%u", host, &port) != 2 || port < 1 || port > 65535 ||
            inet_pton(AF_INET, host, &addr.sin_addr) != 1) goto fail;
        addr.sin_port = htons(port);
        v->udp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (v->udp_fd < 0 || connect(v->udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto fail;
    }
    if (!display) goto start_relay;
    int pipefd[2];
    if (!g_unix_open_pipe(pipefd, FD_CLOEXEC, NULL)) goto fail;
    v->fd = pipefd[0]; v->video_fd = pipefd[1];
    if (!g_unix_set_fd_nonblocking(v->video_fd, TRUE, NULL)) goto fail;
    const char *args[] = {"--no-video-title-show", "--no-video-deco", "--no-snapshot-preview",
                          "--avcodec-hw=none", "--no-drop-late-frames", "--no-skip-frames"};
    v->vlc = libvlc_new(G_N_ELEMENTS(args), args);
    if (!v->vlc) goto fail;
    libvlc_media_t *media = libvlc_media_new_fd(v->vlc, v->fd);
    if (!media) goto fail;
    libvlc_media_add_option(media, ":demux=ts");
    libvlc_media_add_option(media, url ? ":file-caching=1500" : ":file-caching=300");
    v->player = libvlc_media_player_new_from_media(media);
    libvlc_media_release(media);
    if (!v->player) goto fail;
    libvlc_audio_set_mute(v->player, v->muted);
    libvlc_video_set_callbacks(v->player, lock_frame, unlock_frame, display_frame, v);
    libvlc_video_set_format_callbacks(v->player, setup, NULL);
    if (libvlc_media_player_play(v->player) < 0) goto fail;
start_relay:
    v->relay = g_thread_new("video-ts-relay", relay_ts, v);
    v->active = TRUE;
    gtk_widget_queue_draw(v->area);
    return TRUE;
fail:
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot start video / UDP output: %s",
                libvlc_errmsg() ? libvlc_errmsg() : g_strerror(errno));
    video_view_stop(v);
    return FALSE;
}

gboolean video_view_failed(VideoView *v) {
    return g_atomic_int_get(&v->relay_failed) ||
        (v->player && libvlc_media_player_get_state(v->player) == libvlc_Error);
}

void video_view_destroy(VideoView *v) {
    if (v->timer) { g_source_remove(v->timer); v->timer = 0; }
    video_view_stop(v); g_mutex_clear(&v->mutex);
}
