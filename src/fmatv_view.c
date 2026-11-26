#define _POSIX_C_SOURCE 200809L
#include "fmatv_view.h"
#include "aligned_memory.h"
#include "../components/fmatv/analog/light_stack.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>

static gpointer receive_fields(gpointer opaque) {
    VideoView *v = opaque;
    const size_t size = 4*FMATV_WIDTH*FMATV_HEIGHT;
    /* Place the wire flag just before an aligned array of RGB32 pixels. */
    uint32_t *storage = g_malloc(sizeof(uint32_t)+size);
    uint32_t *pixels = storage+1;
    unsigned char *packet = (unsigned char *)pixels-1;
    LightStack *stack = NULL;
    size_t used = 0;
    while (!g_atomic_int_get(&v->stopping)) {
        struct pollfd p = {.fd=v->source_fd, .events=POLLIN};
        int rc = poll(&p,1,100);
        if (!rc || (rc<0 && errno==EINTR)) continue;
        ssize_t n = rc<0 ? -1 : read(p.fd,packet+used,1+size-used);
        if (n<0 && (errno==EAGAIN || errno==EINTR)) continue;
        if (n<=0) {
            if (n<0 || used) g_atomic_int_set(&v->relay_failed,TRUE);
            break;
        }
        used += n;
        if (used!=1+size) continue;
        g_mutex_lock(&v->mutex);
        if (v->framestack_enabled) {
            if (!stack) stack = g_new0(LightStack,1);
            if (v->framestack_reset) light_stack_reset(stack);
            light_stack_add(stack,pixels,(uint32_t *)v->frame);
        } else {
            memcpy(v->frame,pixels,size);
        }
        v->framestack_reset = FALSE;
        v->analog_flags = packet[0];
        v->frame_time = g_get_monotonic_time();
        v->ready = v->dirty = TRUE;
        g_mutex_unlock(&v->mutex);
        used = 0;
    }
    g_free(stack);
    g_free(storage);
    g_atomic_int_set(&v->input_ended,TRUE);
    return NULL;
}

gboolean fmatv_view_start(VideoView *v, int fd, GError **error) {
    video_view_stop(v);
    v->source_fd = fcntl(fd,F_DUPFD_CLOEXEC,3);
    v->frame = linrad_aligned_alloc(32,4*FMATV_WIDTH*FMATV_HEIGHT);
    if (v->source_fd<0 || !v->frame) {
        g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_FAILED,"Cannot open FM-ATV video display");
        video_view_stop(v);
        return FALSE;
    }
    v->width=FMATV_WIDTH; v->height=FMATV_HEIGHT; v->pitch=4*v->width;
    v->aspect=4./3; v->active=v->local_display=v->analog=TRUE;
    v->analog_flags=0;
    g_atomic_int_set(&v->stopping,FALSE);
    g_atomic_int_set(&v->input_ended,FALSE);
    g_atomic_int_set(&v->relay_failed,FALSE);
    v->relay=g_thread_new("fm-atv-display",receive_fields,v);
    return TRUE;
}
