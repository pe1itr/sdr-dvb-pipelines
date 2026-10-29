#define _POSIX_C_SOURCE 200809L
#include "scatter_backend.h"
#include <glib-unix.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>

static void wait_done(ScatterBackend *s) {
    gint64 deadline = g_get_monotonic_time() + 15000000;
    while (!scatter_backend_done(s) && g_get_monotonic_time() < deadline) g_usleep(1000);
    g_assert_true(scatter_backend_done(s));
}

typedef struct { int fd; const char *data; gsize size; } Writer;
static gpointer write_fragments(gpointer data) {
    Writer *w = data;
    for (gsize at = 0; at < w->size;) {
        ssize_t n = write(w->fd, w->data + at, MIN((gsize)137, w->size - at));
        g_assert_cmpint(n, >, 0); at += n;
    }
    /* Deliberately keep the stream open: the timer must recover a picture
     * even when no next PES/EOF arrives from a weak signal. */
    return NULL;
}

int main(int argc, char **argv) {
    g_assert_cmpint(argc, ==, 3);
    signal(SIGPIPE, SIG_IGN);
    char *data = NULL; gsize size;
    g_assert_true(g_file_get_contents(argv[1], &data, &size, NULL));
    int input[2]; g_assert_true(g_unix_open_pipe(input, FD_CLOEXEC, NULL));
    int output = open(argv[2], O_CREAT | O_TRUNC | O_RDWR, 0600);
    g_assert_cmpint(output, >=, 0);
    GError *error = NULL;
    ScatterBackend *s = scatter_backend_start(input[0], output, FALSE, &error);
    g_assert_no_error(error); g_assert_nonnull(s); close(input[0]);
    Writer writer = {.fd=input[1], .data=data, .size=size};
    for (int round = 0; round < 6; ++round) {
        gboolean enabled = round >= 4 || round % 2;
        if (round >= 4) {
            scatter_backend_reset(s);
            g_mutex_lock(&s->mutex);
            g_assert_true(s->enabled);
            g_assert_null(s->image);
            g_assert_cmpuint(s->status.packets_received, ==, 0);
            g_assert_cmpuint(s->queued, ==, 0);
            g_mutex_unlock(&s->mutex);
        } else scatter_backend_set_enabled(s, enabled);
        GThread *thread = g_thread_new("test-fragments", write_fragments, &writer);
        g_thread_join(thread);
        gint64 deadline = g_get_monotonic_time() + 10000000;
        gboolean ready = FALSE;
        do {
            struct stat st; g_assert_cmpint(fstat(output, &st), ==, 0);
            g_mutex_lock(&s->mutex);
            ready = (gsize)st.st_size == size * (round + 1) && (!enabled || s->image != NULL);
            g_assert_cmpuint(s->queued, <=, SCATTER_QUEUE_LIMIT);
            if (!enabled) {
                g_assert_null(s->image);
                g_assert_cmpuint(s->status.packets_received, ==, 0);
                g_assert_cmpuint(s->queued, ==, 0);
            }
            g_mutex_unlock(&s->mutex);
            if (!ready) g_usleep(10000);
        } while (!ready && g_get_monotonic_time() < deadline);
        g_assert_true(ready);
    }
    close(input[1]); wait_done(s);
    g_assert_false(g_atomic_int_get(&s->relay_failed));
    g_assert_false(g_atomic_int_get(&s->recovery_failed));
    g_assert_cmpstr(s->status.provider_name, ==, "PE1ITR");
    g_assert_cmpstr(s->status.service_name, ==, "Scatter test");
    g_assert_cmpuint(s->status.progress_percent, ==, 100);
    g_assert_cmpint(s->width, ==, 320); g_assert_cmpint(s->height, ==, 240);
    g_assert_cmpuint(s->status.decoded_images, >, 0);
    char *received = NULL; gsize received_size;
    g_assert_true(g_file_get_contents(argv[2], &received, &received_size, NULL));
    g_assert_cmpuint(received_size, ==, 6 * size);
    for (int round = 0; round < 6; ++round)
        g_assert_cmpmem(received + round * size, size, data, size);
    g_free(received); scatter_backend_destroy(s); close(output); g_free(data);

    /* Stop must release an idle source and a full, unread player pipe. */
    for (int run = 0; run < 3; ++run) {
        int blocked[2]; g_assert_true(g_unix_open_pipe(blocked, FD_CLOEXEC, NULL));
        int idle[2] = {-1, -1};
        if (run == 2) g_assert_true(g_unix_open_pipe(idle, FD_CLOEXEC, NULL));
        int source = run == 2 ? idle[0] : open("/dev/zero", O_RDONLY);
        s = scatter_backend_start(source, blocked[1], TRUE, &error);
        g_assert_no_error(error); g_assert_nonnull(s); close(source); close(blocked[1]);
        g_usleep(20000);
        gint64 start = g_get_monotonic_time();
        scatter_backend_stop(s); wait_done(s);
        g_assert_cmpint(g_get_monotonic_time() - start, <, 1000000);
        scatter_backend_destroy(s); close(blocked[0]);
        if (idle[1] >= 0) close(idle[1]);
    }
    puts("Scatterfix: direct RGB, metadata, stalled input, live toggles, reset, byte-exact relay and Stop OK");
    return 0;
}
