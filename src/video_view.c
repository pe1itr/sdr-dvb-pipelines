#define _POSIX_C_SOURCE 200809L
#include "video_view.h"
#include "aligned_memory.h"
#include <glib-unix.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "process_control.h"

struct VideoExternalChild {
    VideoView *view, *owner;
    GPid pid;
    guint kill_timer;
};

static void output_controls(VideoView *v) {
    const char *label = v->external ? "Terug in venster" : "Extern op GPU";
    if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(v->output_button)), label))
        gtk_button_set_label(GTK_BUTTON(v->output_button), label);
    gtk_widget_set_sensitive(v->output_button, !v->analog && !v->hdmi_window && v->active && v->local_display &&
        !g_atomic_int_get(&v->stopping) && !g_atomic_int_get(&v->input_ended));
}

static void replace_output(VideoView *v, int fd, gboolean external) {
    g_mutex_lock(&v->output_mutex);
    if (v->video_fd >= 0) close(v->video_fd);
    if (fd >= 0 && g_atomic_int_get(&v->input_ended)) { close(fd); fd = -1; }
    v->video_fd = fd;
    v->external = external;
    ++v->output_generation;
    g_mutex_unlock(&v->output_mutex);
}

static void output_clicked(GtkButton *button, gpointer data) {
    (void)button;
    VideoView *v = data;
    GError *error = NULL;
    if (!video_view_set_external(v, !v->external, &error)) {
        gtk_label_set_text(GTK_LABEL(v->notice), error->message);
        g_clear_error(&error);
    }
}

/* Forward the original TS, including all services and aspect metadata. The
 * relay owns no GTK objects and all waits are interruptible during Stop. */
static gpointer relay_ts(gpointer data) {
    VideoView *v = data;
    unsigned char input[65536], packet[1316];
    size_t pending = 0;
    /* A bounded queue absorbs FFplay startup and ordinary read bursts. Unlike
     * waiting for pipe space, it never holds up SDR/UDP input. The relay alone
     * owns these bytes; switching outputs invalidates them by generation. */
    const size_t capacity = 4 * 1024 * 1024;
    unsigned char *queued = NULL;
    size_t head = 0, used = 0;
    guint queue_generation = 0;
    gboolean eof = FALSE;
    while (!g_atomic_int_get(&v->stopping)) {
        struct pollfd io[2] = {{.fd = eof ? -1 : v->source_fd, .events = POLLIN},
                              {.fd = -1, .events = POLLOUT}};
        g_mutex_lock(&v->output_mutex);
        if (queue_generation != v->output_generation) {
            used = head = 0; queue_generation = v->output_generation;
        }
        while (used && v->external && v->video_fd >= 0) {
            ssize_t n = write(v->video_fd, queued + head, MIN(used, capacity - head));
            if (n > 0) { head = (head + n) % capacity; used -= n; continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) io[1].fd = v->video_fd;
            else used = 0; /* Child watch handles player failure. */
            break;
        }
        g_mutex_unlock(&v->output_mutex);
        if (eof && !used) break;
        int rc = poll(io, 2, 100);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) goto failed;
        if (!rc || !io[0].revents) continue;
        ssize_t n = read(v->source_fd, input, sizeof(input));
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n < 0) goto failed;
        if (!n) { eof = TRUE; g_atomic_int_set(&v->input_ended, TRUE); continue; }
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
        g_mutex_lock(&v->output_mutex);
        guint generation = v->output_generation;
        g_mutex_unlock(&v->output_mutex);
        while (at < (size_t)n) {
            if (g_atomic_int_get(&v->stopping)) goto done;
            /* The nonblocking write holds the lock, so a switch can never
             * close/reuse its descriptor underneath the relay. */
            g_mutex_lock(&v->output_mutex);
            if (v->video_fd < 0 || generation != v->output_generation) {
                g_mutex_unlock(&v->output_mutex); break;
            }
            if (v->external) {
                if (!queued) queued = g_malloc(capacity);
                size_t count = (size_t)n - at;
                if (queue_generation != generation || used + count > capacity) {
                    /* A stalled player loses only its backlog. New TS headers
                     * and a keyframe restore playback when it resumes. */
                    used = head = 0; queue_generation = generation;
                }
                size_t tail = (head + used) % capacity;
                size_t first = MIN(count, capacity - tail);
                memcpy(queued + tail, input + at, first);
                memcpy(queued, input + at + first, count - first);
                used += count;
                g_mutex_unlock(&v->output_mutex); break;
            }
            int output_fd = v->video_fd;
            ssize_t written = write(v->video_fd, input + at, (size_t)n - at);
            int saved_errno = errno;
            g_mutex_unlock(&v->output_mutex);
            if (written < 0 && saved_errno == EINTR) continue;
            /* Keep all data, including sparse frames, for embedded playback. */
            if (written < 0 && (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK)) {
                struct pollfd output = {.fd = output_fd, .events = POLLOUT};
                poll(&output, 1, 100);
                /* A switch may have replaced the fd during poll. Recheck
                 * the generation under the lock before the next write. */
                continue;
            }
            if (written <= 0) goto failed;
            at += written;
        }
    }
    goto done;
failed:
    g_atomic_int_set(&v->relay_failed, TRUE);
done:
    g_free(queued);
    g_atomic_int_set(&v->input_ended, TRUE);
    g_mutex_lock(&v->output_mutex);
    if (v->video_fd >= 0) { close(v->video_fd); v->video_fd = -1; }
    ++v->output_generation;
    g_mutex_unlock(&v->output_mutex);
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
        linrad_aligned_free(pixels); linrad_aligned_free(frame); return 0;
    }
    g_mutex_lock(&v->mutex);
    linrad_aligned_free(v->pixels); linrad_aligned_free(v->frame);
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
    v->frame_time = g_get_monotonic_time();
    v->ready = v->dirty = TRUE;
    g_mutex_unlock(&v->mutex);
}

const char *video_view_hdmi_state(VideoView *v) {
    static const char *const states[] = {"No Signal", "Init", "Locked", "Video"};
    return v->hdmi_window ? states[v->hdmi_state] : "Off";
}

void video_view_hdmi_update(VideoView *v, gboolean detected, gboolean locked) {
    if (!v->hdmi_window) return;
    gint64 now = g_get_monotonic_time();
    int previous = v->hdmi_state;
    if (!detected && !locked) {
        v->hdmi_state = HDMI_NO_SIGNAL;
    } else if (previous == HDMI_NO_SIGNAL) {
        v->hdmi_state = HDMI_INIT;
        v->hdmi_acquired = now;
    } else if (!locked) {
        v->hdmi_state = HDMI_INIT;
        if (previous != HDMI_INIT) v->hdmi_acquired = now;
    } else if (previous == HDMI_INIT) {
        if (now - v->hdmi_since >= 350000) v->hdmi_state = HDMI_LOCKED;
    } else {
        g_mutex_lock(&v->mutex);
        gboolean picture = v->ready && v->frame_time >= v->hdmi_acquired &&
                           now - v->frame_time < 2 * G_USEC_PER_SEC;
        g_mutex_unlock(&v->mutex);
        if (!picture) v->hdmi_state = HDMI_LOCKED;
        else if (now - v->hdmi_since >= 350000) v->hdmi_state = HDMI_VIDEO;
    }
    if ((int)v->hdmi_state != previous) {
        v->hdmi_since = now;
        gtk_widget_queue_draw(v->hdmi_area);
    }
}

static gboolean draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    VideoView *v = data;
    double w = gtk_widget_get_allocated_width(widget), h = gtk_widget_get_allocated_height(widget);
    if (widget == v->hdmi_area && v->hdmi_state != HDMI_VIDEO) {
        cairo_set_source_rgb(cr, 0, 0, 0.68);
        cairo_paint(cr);
        PangoLayout *label = gtk_widget_create_pango_layout(widget, video_view_hdmi_state(v));
        PangoFontDescription *font = pango_font_description_from_string("Sans Bold");
        pango_font_description_set_absolute_size(font, MAX(16, h / 12) * PANGO_SCALE);
        pango_layout_set_font_description(label, font);
        int tw, th;
        pango_layout_get_pixel_size(label, &tw, &th);
        cairo_move_to(cr, (w - tw) / 2, (h - th) / 2);
        cairo_set_source_rgb(cr, 1, 1, 1);
        pango_cairo_show_layout(cr, label);
        pango_font_description_free(font);
        g_object_unref(label);
        return FALSE;
    }
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
            v->active ? (v->external ? "Video in externe speler" :
                v->local_display ? "Waiting for video…" : "Streaming via UDP") : "Received video");
        int tw, th; pango_layout_get_pixel_size(text, &tw, &th);
        cairo_set_source_rgb(cr, .65, .65, .65);
        cairo_move_to(cr, (w - tw) / 2, (h - th) / 2);
        pango_cairo_show_layout(cr, text); g_object_unref(text);
    }
    g_mutex_unlock(&v->mutex);
    return FALSE;
}

gboolean video_view_hdmi_monitor(VideoView *v, int monitor) {
    if (!monitor) {
        if (v->hdmi_window) gtk_widget_destroy(v->hdmi_window);
        v->hdmi_window = v->hdmi_area = v->hdmi_overlay = NULL;
        v->hdmi_monitor = 0;
        return TRUE;
    }
    GdkScreen *screen = gtk_widget_get_screen(v->area);
    if (monitor < 1 || monitor > gdk_display_get_n_monitors(gdk_screen_get_display(screen)))
        return FALSE;
    if (!v->hdmi_window) {
        v->hdmi_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(v->hdmi_window), "Linrad DVB HDMI");
        gtk_window_set_decorated(GTK_WINDOW(v->hdmi_window), FALSE);
        gtk_window_set_deletable(GTK_WINDOW(v->hdmi_window), FALSE);
        v->hdmi_area = gtk_drawing_area_new();
        v->hdmi_overlay = gtk_overlay_new();
        gtk_container_add(GTK_CONTAINER(v->hdmi_window), v->hdmi_overlay);
        gtk_container_add(GTK_CONTAINER(v->hdmi_overlay), v->hdmi_area);
        g_signal_connect(v->hdmi_area, "draw", G_CALLBACK(draw), v);
        v->hdmi_state = HDMI_NO_SIGNAL;
    }
    gtk_window_fullscreen_on_monitor(GTK_WINDOW(v->hdmi_window), screen, monitor - 1);
    gtk_widget_show_all(v->hdmi_window);
    v->hdmi_monitor = monitor;
    return TRUE;
}

static gboolean tick(gpointer data) {
    VideoView *v = data;
    if (v->player) {
        /* Audio outputs can restore a per-application volume asynchronously.
         * Set unity once after audio starts, matching FFplay's -volume 100;
         * later system mixer changes remain under the user's control. */
        if (!v->volume_initialized && libvlc_audio_get_track(v->player)>=0 &&
            libvlc_media_player_get_state(v->player)==libvlc_Playing &&
            libvlc_audio_set_volume(v->player,100)==0)
            v->volume_initialized=TRUE;
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
    if (dirty) {
        gtk_widget_queue_draw(v->area);
        if (v->hdmi_window && v->hdmi_state == HDMI_VIDEO) gtk_widget_queue_draw(v->hdmi_area);
    }
    output_controls(v);
    return G_SOURCE_CONTINUE;
}

void video_view_init(VideoView *v) {
    g_mutex_init(&v->mutex); g_mutex_init(&v->output_mutex);
    v->fd = v->source_fd = v->video_fd = v->udp_fd = -1;
}

GtkWidget *video_view_create(VideoView *v) {
    video_view_init(v);
    v->gpu_program = g_strdup("/usr/bin/ffplay"); v->gpu_device = g_strdup("NVIDIA");
    v->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(v->area, 320, 180);
    gtk_widget_set_hexpand(v->area, TRUE); gtk_widget_set_vexpand(v->area, TRUE);
    g_signal_connect(v->area, "draw", G_CALLBACK(draw), v);
    v->timer = g_timeout_add(33, tick, v);
    v->container = gtk_frame_new("Received video");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_add(GTK_CONTAINER(v->container), box);
    gtk_box_pack_start(GTK_BOX(box), v->area, TRUE, TRUE, 0);
    v->output_button = gtk_button_new_with_label("Extern op GPU");
    gtk_widget_set_tooltip_text(v->output_button,
        "Wissel tussen dit venster en FFplay op de NVIDIA-GPU. De ontvangst blijft lopen.");
    gtk_box_pack_start(GTK_BOX(box), v->output_button, FALSE, FALSE, 0);
    v->notice = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(v->notice), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(v->notice), 35);
    gtk_box_pack_start(GTK_BOX(box), v->notice, FALSE, FALSE, 0);
    g_signal_connect(v->output_button, "clicked", G_CALLBACK(output_clicked), v);
    output_controls(v);
    return v->container;
}

void video_view_set_muted(VideoView *v, gboolean muted) {
    gboolean changed = v->muted != muted;
    v->muted = muted;
    if (v->player) libvlc_audio_set_mute(v->player, muted);
    if (changed && v->external) {
        GError *error = NULL;
        /* FFplay has no control API for volume. Reopen only the player; the
         * stream, radio and service selection remain live. */
        if (!video_view_set_external(v, TRUE, &error)) {
            gtk_label_set_text(GTK_LABEL(v->notice), error->message);
            g_clear_error(&error);
        }
    }
    if (v->container) gtk_frame_set_label(GTK_FRAME(v->container),
        muted ? "Received video · Muted (M)" : "Received video");
}

static gboolean kill_external(gpointer data) {
    VideoExternalChild *child = data;
    child->kill_timer = 0;
    process_stop(child->pid, TRUE, TRUE);
    return G_SOURCE_REMOVE;
}

static void stop_external(VideoView *v) {
    VideoExternalChild *child = v->external_child;
    if (!child) return;
    v->external_child = NULL;
    child->view = NULL; /* Reaper must never access a stopped/destroyed view. */
    process_stop(child->pid, TRUE, FALSE);
    child->kill_timer = g_timeout_add(1000, kill_external, child);
}

static void stop_player(VideoView *v) {
    if (v->player) {
        libvlc_media_player_stop(v->player);
        libvlc_media_player_release(v->player); v->player = NULL;
    }
    if (v->fd >= 0) { close(v->fd); v->fd = -1; }
    g_mutex_lock(&v->mutex);
    linrad_aligned_free(v->pixels); linrad_aligned_free(v->frame); v->pixels = v->frame = NULL;
    v->ready = FALSE; v->dirty = TRUE; v->aspect = 0;
    g_mutex_unlock(&v->mutex);
}

void video_view_stop(VideoView *v) {
    g_atomic_int_set(&v->stopping, TRUE);
    if (v->relay) { g_thread_join(v->relay); v->relay = NULL; }
    replace_output(v, -1, FALSE);
    stop_external(v);
    stop_player(v);
    if (v->vlc) { libvlc_release(v->vlc); v->vlc = NULL; }
    if (v->source_fd >= 0) { close(v->source_fd); v->source_fd = -1; }
    if (v->udp_fd >= 0) { close(v->udp_fd); v->udp_fd = -1; }
    v->analog = v->active = FALSE; v->program = 0;
    if (v->output_button) output_controls(v);
    if (v->notice) gtk_label_set_text(GTK_LABEL(v->notice), "");
}

static gboolean start_player(VideoView *v, guint16 program) {
    libvlc_media_t *media = libvlc_media_new_fd(v->vlc, v->fd);
    if (!media) return FALSE;
    char option[64];
    libvlc_media_add_option(media, ":demux=ts");
    g_snprintf(option, sizeof(option), ":file-caching=%u", v->caching);
    libvlc_media_add_option(media, option);
    g_snprintf(option, sizeof(option), ":program=%u", program);
    libvlc_media_add_option(media, option);
    v->player = libvlc_media_player_new_from_media(media);
    libvlc_media_release(media);
    if (!v->player) return FALSE;
    v->volume_initialized=FALSE;
    /* PipeWire's output may exist before its stream has started. Setting its
     * volume here can dereference an uninitialized stream when VLC is reopened.
     * tick() sets unity once audio is playing, after output initialization. */
    libvlc_audio_set_mute(v->player, v->muted);
    libvlc_video_set_callbacks(v->player, lock_frame, unlock_frame, display_frame, v);
    libvlc_video_set_format_callbacks(v->player, setup, NULL);
    if (libvlc_media_player_play(v->player) < 0) return FALSE;
    v->program = program;
    return TRUE;
}

void video_view_set_gpu_player(VideoView *v, const char *program, const char *device) {
    g_free(v->gpu_program); g_free(v->gpu_device);
    v->gpu_program = g_strdup(program && *program ? program : "/usr/bin/ffplay");
    v->gpu_device = g_strdup(device && *device ? device : "NVIDIA");
}

static void external_exited(GPid pid, gint result, gpointer data) {
    VideoExternalChild *child = data;
    if (child->kill_timer) g_source_remove(child->kill_timer);
    if (child->owner)
        child->owner->external_children = g_list_remove(child->owner->external_children, child);
    VideoView *v = child->view;
    if (v && v->external_child == child) {
        v->external_child = NULL;
        if (v->active && !g_atomic_int_get(&v->stopping) && !g_atomic_int_get(&v->input_ended)) {
            GError *error = NULL;
            if (video_view_set_external(v, FALSE, &error)) {
                gtk_label_set_text(GTK_LABEL(v->notice),
                    WIFEXITED(result) && WEXITSTATUS(result) == 0 ?
                    "Externe speler gesloten; terug in venster." :
                    "Externe speler gestopt; terug in venster.");
            } else {
                gtk_label_set_text(GTK_LABEL(v->notice), error->message);
                g_clear_error(&error);
            }
        }
    }
    g_spawn_close_pid(pid); g_free(child);
}

static void external_setup(gpointer unused) {
    (void)unused;
    if (setsid() < 0) _exit(126);
    signal(SIGPIPE, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
}

gboolean video_view_set_external(VideoView *v, gboolean external, GError **error) {
    if (v->analog || !v->active || !v->local_display || g_atomic_int_get(&v->stopping) ||
        g_atomic_int_get(&v->input_ended)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Schakelen kan tijdens ontvangst met de ingebouwde speler.");
        return FALSE;
    }
    int pipefd[2];
    if (!g_unix_open_pipe(pipefd, FD_CLOEXEC, error)) return FALSE;
    if (!g_unix_set_fd_nonblocking(pipefd[1], TRUE, error)) {
        close(pipefd[0]); close(pipefd[1]); return FALSE;
    }
    VideoExternalChild *child = NULL;
    g_printerr("[player] Switching to %s player\n", external ? "GPU" : "embedded VLC");
    if (external) {
        char *device = g_strdup_printf("device=%s", v->gpu_device);
        char *video = g_strdup_printf("p:%u:v:0", v->program);
        char *audio = g_strdup_printf("p:%u:a:0", v->program);
        const char *argv[40]; unsigned count = 0;
        argv[count++] = v->gpu_program;
        argv[count++] = "-hide_banner"; argv[count++] = "-loglevel"; argv[count++] = "warning";
        argv[count++] = "-autoexit";
        argv[count++] = "-threads"; argv[count++] = "1";
        argv[count++] = "-filter_threads"; argv[count++] = "1";
        argv[count++] = "-hwaccel"; argv[count++] = "cuda";
        argv[count++] = "-vulkan_params"; argv[count++] = device;
        argv[count++] = "-window_title"; argv[count++] = "Received video · GPU";
        argv[count++] = "-volume"; argv[count++] = v->muted ? "0" : "100";
        if (v->program) {
            argv[count++] = "-vst"; argv[count++] = video;
            argv[count++] = "-ast"; argv[count++] = audio;
        }
        argv[count++] = "-f"; argv[count++] = "mpegts";
        argv[count++] = "-i"; argv[count++] = "pipe:0"; argv[count] = NULL;
        child = g_new0(VideoExternalChild, 1);
        int nullfd = open("/dev/null", O_WRONLY | O_CLOEXEC);
        gboolean spawned = nullfd >= 0 && g_spawn_async_with_fds(NULL, (char **)argv, NULL,
            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, external_setup, NULL,
            &child->pid, pipefd[0], nullfd, STDERR_FILENO, error);
        if (nullfd >= 0) close(nullfd);
        else g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot open /dev/null: %s", g_strerror(errno));
        g_free(device); g_free(video); g_free(audio);
        if (!spawned) { g_free(child); close(pipefd[0]); close(pipefd[1]); return FALSE; }
    }
    /* Commit only after a new external process has successfully exec'd. Stop
     * VLC before supplying FFplay any TS; no simultaneous video decoders. */
    replace_output(v, -1, external);
    stop_player(v);
    stop_external(v);
    v->fd = pipefd[0];
    if (external) {
        child->view = child->owner = v;
        v->external_child = child;
        v->external_children = g_list_prepend(v->external_children, child);
        g_child_watch_add(child->pid, external_exited, child);
    } else if (!start_player(v, v->program)) {
        close(pipefd[1]); stop_player(v);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Ingebouwde speler kon niet starten.");
        output_controls(v); return FALSE;
    }
    replace_output(v, pipefd[1], external);
    gtk_label_set_text(GTK_LABEL(v->notice), external ?
        "Ontvangst loopt door. Sluit FFplay om terug te keren." : "");
    output_controls(v);
    gtk_widget_queue_draw(v->area);
    return TRUE;
}

gboolean video_view_select_program(VideoView *v, guint16 program, GError **error) {
    if (!v->active || !v->local_display || (!v->player && !v->external) || !program) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Program selection requires the embedded player.");
        return FALSE;
    }
    if (program == v->program) return TRUE;
    if (v->external) {
        guint16 old_program = v->program;
        v->program = program;
        if (video_view_set_external(v, TRUE, error)) return TRUE;
        v->program = old_program; return FALSE;
    }
    /* LibVLC 3 has no public program-switch API. Reopen only its demux/player
     * with :program; retain the live input, relay and unmodified UDP output.
     * The original read fd stays open while VLC closes its own duplicate. */
    libvlc_media_player_stop(v->player);
    libvlc_media_player_release(v->player); v->player = NULL;
    g_mutex_lock(&v->mutex);
    v->ready = FALSE; v->dirty = TRUE; v->aspect = 0;
    g_mutex_unlock(&v->mutex);
    gtk_widget_queue_draw(v->area);
    if (start_player(v, program)) return TRUE;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot play program %u: %s",
        program, libvlc_errmsg() ? libvlc_errmsg() : "VLC playback failed");
    return FALSE;
}

gboolean video_view_start(VideoView *v, int fd, const char *url,
                          const char *udp_destination, gboolean display, GError **error) {
    video_view_stop(v);
    v->local_display = display;
    v->caching = url ? 1500 : 300;
    g_atomic_int_set(&v->stopping, FALSE); g_atomic_int_set(&v->relay_failed, FALSE);
    g_atomic_int_set(&v->input_ended, FALSE);
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
    /* Frame threading holds pictures until more frames arrive. During broken
     * reception those frames may never arrive before Stop closes the stream. */
    const char *args[] = {"--no-video-title-show", "--no-video-deco", "--no-snapshot-preview",
#ifdef __linux__
                          /* Prefer VLC's buffered PulseAudio output (also served by
                           * pipewire-pulse). Keep automatic fallback on other setups. */
                          "--aout=pulse,any",
#endif
                          "--avcodec-hw=none", "--avcodec-threads=1",
                          "--no-drop-late-frames", "--no-skip-frames"};
    v->vlc = libvlc_new(G_N_ELEMENTS(args), args);
    if (!v->vlc) goto fail;
    if (!start_player(v, 0)) goto fail;
start_relay:
    v->relay = g_thread_new("video-ts-relay", relay_ts, v);
    v->active = TRUE;
    if (v->output_button) output_controls(v);
    if (v->area) gtk_widget_queue_draw(v->area);
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
    video_view_hdmi_monitor(v, 0);
    if (v->timer) { g_source_remove(v->timer); v->timer = 0; }
    video_view_stop(v);
    /* Destruction must not depend on running the GTK loop for another second. */
    for (GList *p = v->external_children; p; p = p->next) {
        VideoExternalChild *child = p->data;
        child->owner = child->view = NULL;
        if (child->kill_timer) { g_source_remove(child->kill_timer); child->kill_timer = 0; }
        process_stop(child->pid, TRUE, TRUE);
    }
    g_list_free(v->external_children); v->external_children = NULL;
    g_free(v->gpu_program); g_free(v->gpu_device);
    g_mutex_clear(&v->mutex); g_mutex_clear(&v->output_mutex);
}
