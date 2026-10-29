/* Real MPEG-TS decode, UDP duplication and fixed GTK allocation. */
#define _POSIX_C_SOURCE 200809L
#include "video_view.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static void iterate(void) {
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(10000);
}

static void check_udp_only(VideoView *v, int receiver, const char *destination, gboolean network_input) {
    unsigned char payload[188 * 8], received[sizeof(payload)], discard[65536];
    while (recv(receiver, discard, sizeof(discard), 0) > 0) {}
    for (unsigned i = 0; i < sizeof(payload); ++i) payload[i] = i % 188 ? (unsigned char)i : 0x47;
    int source[2] = {-1, -1};
    char *url = NULL;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (network_input) {
        source[1] = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        g_assert_cmpint(bind(source[1], (struct sockaddr *)&addr, sizeof(addr)), ==, 0);
        socklen_t length = sizeof(addr);
        g_assert_cmpint(getsockname(source[1], (struct sockaddr *)&addr, &length), ==, 0);
        close(source[1]);
        url = g_strdup_printf("udp://@127.0.0.1:%u", ntohs(addr.sin_port));
        source[1] = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    } else g_assert_cmpint(pipe(source), ==, 0);
    GError *error = NULL;
    g_assert_true(video_view_start(v, source[0], url, destination, FALSE, &error));
    g_assert_no_error(error); g_assert_null(v->player);
    if (network_input) {
        g_assert_cmpint(sendto(source[1], payload, sizeof(payload), 0, (struct sockaddr *)&addr, sizeof(addr)), ==, sizeof(payload));
    } else {
        /* Reads split in the middle of a TS packet must preserve every byte. */
        g_assert_cmpint(write(source[1], payload, 37), ==, 37);
        for (int i = 0; i < 15; ++i) iterate();
        g_assert_cmpint(recv(receiver, discard, sizeof(discard), 0), ==, -1);
        g_assert_cmpint(write(source[1], payload + 37, sizeof(payload) - 37), ==, sizeof(payload) - 37);
    }
    size_t used = 0;
    gint64 until = g_get_monotonic_time() + G_USEC_PER_SEC;
    while (used < sizeof(received) && g_get_monotonic_time() < until) {
        iterate();
        ssize_t n = recv(receiver, received + used, sizeof(received) - used, 0);
        if (n > 0) { g_assert_cmpint(n % 188, ==, 0); used += n; }
    }
    g_assert_cmpmem(received, used, payload, sizeof(payload));
    g_assert_false(v->ready); g_assert_false(video_view_failed(v));
    video_view_stop(v);
    if (source[0] >= 0) close(source[0]);
    close(source[1]); g_free(url);
}

int main(int argc, char **argv) {
    g_assert_cmpint(argc, ==, 6);
    gtk_init(NULL, NULL);
    VideoView v = {0};
    GtkWidget *window = gtk_offscreen_window_new();
    gtk_window_set_default_size(GTK_WINDOW(window), 640, 480);
    gtk_container_add(GTK_CONTAINER(window), video_view_create(&v));
    gtk_widget_show_all(window);
    for (int i = 0; i < 10; ++i) iterate();
    int width = gtk_widget_get_allocated_width(v.area), height = gtk_widget_get_allocated_height(v.area);
    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    g_assert_cmpint(sock, >=, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    g_assert_cmpint(bind(sock, (struct sockaddr *)&addr, sizeof(addr)), ==, 0);
    socklen_t length = sizeof(addr);
    g_assert_cmpint(getsockname(sock, (struct sockaddr *)&addr, &length), ==, 0);
    char *destination = g_strdup_printf("127.0.0.1:%u", ntohs(addr.sin_port));
    FILE *forwarded = fopen(argv[5], "wb"); g_assert_nonnull(forwarded);
    for (int i = 1; i <= 4; ++i) {
        int fd = open(argv[i], O_RDONLY | O_CLOEXEC); g_assert_cmpint(fd, >=, 0);
        GError *error = NULL;
        g_assert_true(video_view_start(&v, fd, NULL, destination, TRUE, &error));
        g_assert_no_error(error); close(fd);
        gint64 until = g_get_monotonic_time() + 6 * G_USEC_PER_SEC;
        gboolean ready = FALSE; unsigned packets = 0;
        while (g_get_monotonic_time() < until) {
            iterate();
            unsigned char bytes[65536]; ssize_t n;
            while ((n = recv(sock, bytes, sizeof(bytes), 0)) > 0) {
                g_assert_cmpint(n % 188, ==, 0);
                for (ssize_t p = 0; p < n; p += 188) g_assert_cmpint(bytes[p], ==, 0x47);
                if (i == 1) g_assert_cmpuint(fwrite(bytes, 1, n, forwarded), ==, (size_t)n);
                ++packets;
            }
            g_mutex_lock(&v.mutex); ready = v.ready; g_mutex_unlock(&v.mutex);
            g_assert_false(video_view_failed(&v));
            if (ready && v.aspect > 0 && packets > 10) break;
        }
        g_assert_true(ready); g_assert_cmpuint(packets, >, 10);
        g_assert_cmpint(gtk_widget_get_allocated_width(v.area), ==, width);
        g_assert_cmpint(gtk_widget_get_allocated_height(v.area), ==, height);
        /* Verify actual painted pixels and letterboxing, not only decoder state. */
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
        cairo_t *cr = cairo_create(surface); gboolean handled;
        g_signal_emit_by_name(v.area, "draw", cr, &handled);
        cairo_surface_flush(surface);
        unsigned char *pixels = cairo_image_surface_get_data(surface);
        int stride = cairo_image_surface_get_stride(surface);
        uint32_t center = *(uint32_t *)(pixels + height / 2 * stride + width / 2 * 4);
        g_assert_cmpuint((center >> 8) & 255, >, 80); /* Green test picture. */
        g_assert_cmpuint((center >> 16) & 255, <, 30);
        if (i % 2 == 0) {
            uint32_t top = *(uint32_t *)(pixels + 5 * stride + width / 2 * 4);
            g_assert_cmpuint(top & 0xffffff, ==, 0); /* Wide picture: black top bar. */
        }
        int colored_width = 0, colored_height = 0;
        for (int x = 0; x < width; ++x)
            if ((*(uint32_t *)(pixels + height / 2 * stride + x * 4) & 0xff00) > 0x5000) ++colored_width;
        for (int y = 0; y < height; ++y)
            if ((*(uint32_t *)(pixels + y * stride + width / 2 * 4) & 0xff00) > 0x5000) ++colored_height;
        double actual_aspect = (double)colored_width / colored_height;
        g_printerr("Video %d: buffer=%ux%u display=%.6f colored=%dx%d panel=%dx%d\n",
            i, v.width, v.height, v.aspect, colored_width, colored_height, width, height);
        g_assert_cmpfloat_with_epsilon(actual_aspect, i % 2 ? 4.0 / 3 : 16.0 / 9, .02);
        cairo_destroy(cr); cairo_surface_destroy(surface);
        video_view_stop(&v);
        g_assert_false(v.ready); g_assert_null(v.player); g_assert_cmpint(v.fd, ==, -1);
    }
    check_udp_only(&v, sock, destination, FALSE);
    check_udp_only(&v, sock, destination, TRUE);
    /* Stopping before any TS arrives must not hang on the input pipe. */
    int pipefd[2]; g_assert_cmpint(pipe(pipefd), ==, 0);
    GError *error = NULL;
    g_assert_true(video_view_start(&v, pipefd[0], NULL, NULL, TRUE, &error));
    close(pipefd[0]);
    for (int i = 0; i < 20; ++i) iterate();
    video_view_stop(&v); close(pipefd[1]);
    video_view_destroy(&v); gtk_widget_destroy(window);
    fclose(forwarded); close(sock); g_free(destination);
    return 0;
}
