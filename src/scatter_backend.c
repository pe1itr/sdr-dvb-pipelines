#define _POSIX_C_SOURCE 200809L
#include "scatter_backend.h"
#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

static void image_ready(void *data, const SfImage *image) {
    ScatterBackend *s = data;
    if (image->width <= 0 || image->height <= 0 || image->width > 8192 ||
        image->height > 8192 || image->stride < image->width * 3) return;
    /* Library memory is borrowed only for this callback. */
    GBytes *copy = g_bytes_new(image->pixels, (size_t)image->stride * image->height);
    g_mutex_lock(&s->mutex);
    if (!s->enabled || s->worker_generation != s->generation || g_atomic_int_get(&s->stopping)) {
        g_mutex_unlock(&s->mutex); g_bytes_unref(copy); return;
    }
    g_clear_pointer(&s->image, g_bytes_unref);
    s->image = copy; s->width = image->width; s->height = image->height; s->stride = image->stride;
    g_mutex_unlock(&s->mutex);
}

static void status_ready(void *data, const SfRecoveryStatus *status) {
    ScatterBackend *s = data;
    g_mutex_lock(&s->mutex);
    if (s->enabled && s->worker_generation == s->generation && !g_atomic_int_get(&s->stopping))
        s->status = *status;
    g_mutex_unlock(&s->mutex);
}

static void clear_queue(ScatterBackend *s) {
    g_queue_clear_full(&s->queue, (GDestroyNotify)g_bytes_unref); s->queued = 0;
}

static gpointer recover(gpointer data) {
    ScatterBackend *s = data;
    SfRecoveryConfig config; sf_recovery_config_init(&config);
    config.image = image_ready; config.status = status_ready; config.userdata = s;
    /* Batch analysis at 2 Hz, including incomplete PES after reception stalls. */
    config.analyze_on_feed = 0;
    SfRecovery *recovery = NULL;
    guint64 generation = 0;
    gint64 next_analysis = g_get_monotonic_time() + 500000;
    while (!g_atomic_int_get(&s->stopping)) {
        g_mutex_lock(&s->mutex);
        while (!s->eof && !g_atomic_int_get(&s->stopping) && generation == s->generation &&
               (!s->enabled || g_atomic_int_get(&s->recovery_failed) || g_queue_is_empty(&s->queue))) {
            if (!s->enabled || g_atomic_int_get(&s->recovery_failed)) g_cond_wait(&s->wake, &s->mutex);
            else if (!g_cond_wait_until(&s->wake, &s->mutex, next_analysis)) break;
        }
        gboolean changed = generation != s->generation;
        generation = s->worker_generation = s->generation;
        gboolean enabled = s->enabled;
        GBytes *chunk = g_queue_pop_head(&s->queue);
        if (chunk) s->queued -= g_bytes_get_size(chunk);
        gboolean gap = s->gap; s->gap = FALSE;
        gboolean eof = s->eof && !chunk;
        g_mutex_unlock(&s->mutex);
        if (g_atomic_int_get(&s->stopping)) { if (chunk) g_bytes_unref(chunk); break; }
        if (changed) {
            sf_recovery_destroy(recovery); recovery = NULL;
            if (enabled) recovery = sf_recovery_create(&config);
            next_analysis = g_get_monotonic_time() + 500000;
        }
        if (!enabled) { if (chunk) g_bytes_unref(chunk); if (eof) break; continue; }
        if (gap && recovery) sf_recovery_reset(recovery);
        int ok = recovery != NULL;
        if (chunk) {
            gsize size; const guint8 *bytes = g_bytes_get_data(chunk, &size);
            if (ok) ok = sf_recovery_feed(recovery, bytes, size);
            g_bytes_unref(chunk);
        }
        if (ok && eof) ok = sf_recovery_finish(recovery);
        else if (ok && g_get_monotonic_time() >= next_analysis) {
            ok = sf_recovery_analyze(recovery);
            next_analysis = g_get_monotonic_time() + 500000;
        }
        if (!ok) {
            sf_recovery_destroy(recovery); recovery = NULL;
            g_mutex_lock(&s->mutex);
            if (generation == s->generation) {
                g_atomic_int_set(&s->recovery_failed, TRUE); clear_queue(s);
            }
            g_mutex_unlock(&s->mutex);
        }
        if (eof) break;
    }
    sf_recovery_destroy(recovery);
    g_mutex_lock(&s->mutex); clear_queue(s); g_mutex_unlock(&s->mutex);
    g_atomic_int_set(&s->worker_done, TRUE);
    return NULL;
}

static gpointer relay(gpointer data) {
    ScatterBackend *s = data;
    guint8 bytes[65536];
    while (!g_atomic_int_get(&s->stopping)) {
        struct pollfd p = {.fd=s->input, .events=POLLIN};
        int rc = poll(&p, 1, 100);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) goto failed;
        if (!rc) continue;
        ssize_t n = read(s->input, bytes, sizeof(bytes));
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n < 0) goto failed;
        if (!n) break;
        ts_info_backend_feed(s->information, bytes, n);
        g_mutex_lock(&s->mutex);
        if (s->enabled && !g_atomic_int_get(&s->worker_done) && !g_atomic_int_get(&s->recovery_failed)) {
            GBytes *chunk = g_bytes_new(bytes, n);
            if (s->queued + n > SCATTER_QUEUE_LIMIT) {
                s->dropped += s->queued;
                clear_queue(s); s->gap = TRUE;
            }
            g_queue_push_tail(&s->queue, chunk); s->queued += n;
            g_cond_signal(&s->wake);
        }
        g_mutex_unlock(&s->mutex);
        size_t at = 0;
        while (at < (size_t)n && !g_atomic_int_get(&s->stopping)) {
            p = (struct pollfd){.fd=s->output, .events=POLLOUT};
            rc = poll(&p, 1, 100);
            if (rc < 0 && errno == EINTR) continue;
            if (rc < 0) goto failed;
            if (!rc) continue;
            ssize_t written = write(s->output, bytes + at, n - at);
            if (written < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (written <= 0) goto failed;
            at += written;
        }
    }
    goto done;
failed:
    g_atomic_int_set(&s->relay_failed, TRUE);
done:
    ts_info_backend_finish(s->information);
    close(s->output); s->output = -1;
    g_mutex_lock(&s->mutex); s->eof = TRUE; g_cond_signal(&s->wake); g_mutex_unlock(&s->mutex);
    g_atomic_int_set(&s->relay_done, TRUE);
    return NULL;
}

ScatterBackend *scatter_backend_start(int input, int output, gboolean enabled, GError **error) {
    ScatterBackend *s = g_new0(ScatterBackend, 1);
    g_mutex_init(&s->mutex); g_cond_init(&s->wake); g_queue_init(&s->queue);
    s->status.video_pid = -1; s->enabled = enabled; s->generation = 1;
    s->input = fcntl(input, F_DUPFD_CLOEXEC, 3);
    s->output = fcntl(output, F_DUPFD_CLOEXEC, 3);
    if (s->input < 0 || s->output < 0 || !g_unix_set_fd_nonblocking(s->output, TRUE, NULL)) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Cannot start TSscatterfix: %s", g_strerror(errno));
        scatter_backend_destroy(s); return NULL;
    }
    s->information = ts_info_backend_new();
    s->worker = g_thread_new("scatter-recovery", recover, s);
    s->relay = g_thread_new("scatter-ts-relay", relay, s);
    return s;
}

/* Caller holds mutex; generation prevents old callbacks from restoring state. */
static void reset_recovery(ScatterBackend *s) {
    ++s->generation;
    clear_queue(s); s->gap = FALSE; s->dropped = 0;
    memset(&s->status, 0, sizeof(s->status)); s->status.video_pid = -1;
    g_clear_pointer(&s->image, g_bytes_unref);
    s->width = s->height = s->stride = 0;
    g_atomic_int_set(&s->recovery_failed, FALSE);
    g_cond_signal(&s->wake);
}

void scatter_backend_reset(ScatterBackend *s) {
    if (!s) return;
    g_mutex_lock(&s->mutex);
    if (s->enabled && !g_atomic_int_get(&s->stopping)) reset_recovery(s);
    g_mutex_unlock(&s->mutex);
}

void scatter_backend_set_enabled(ScatterBackend *s, gboolean enabled) {
    if (!s) return;
    g_mutex_lock(&s->mutex);
    if (s->enabled != enabled) {
        s->enabled = enabled;
        reset_recovery(s);
    }
    g_mutex_unlock(&s->mutex);
}

void scatter_backend_stop(ScatterBackend *s) {
    if (!s) return;
    g_atomic_int_set(&s->stopping, TRUE);
    ts_info_backend_stop(s->information);
    g_mutex_lock(&s->mutex); g_cond_signal(&s->wake); g_mutex_unlock(&s->mutex);
}

gboolean scatter_backend_done(ScatterBackend *s) {
    return !s || (g_atomic_int_get(&s->relay_done) && g_atomic_int_get(&s->worker_done) && ts_info_backend_done(s->information));
}

void scatter_backend_destroy(ScatterBackend *s) {
    if (!s) return;
    scatter_backend_stop(s);
    if (s->relay) g_thread_join(s->relay);
    if (s->worker) g_thread_join(s->worker);
    if (s->input >= 0) close(s->input);
    if (s->output >= 0) close(s->output);
    ts_info_backend_free(s->information);
    clear_queue(s); g_clear_pointer(&s->image, g_bytes_unref);
    g_cond_clear(&s->wake); g_mutex_clear(&s->mutex); g_free(s);
}
