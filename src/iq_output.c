#define _POSIX_C_SOURCE 200809L
#include "iq_output.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

struct IQOutput {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t thread;
    unsigned char *data;
    size_t capacity, head, count, peak, write_limit;
    uint64_t accepted, written, dropped, write_stalls;
    int fd, flags, stop, error, drain;
};

static void *writer(void *opaque) {
    IQOutput *q = opaque;
    pthread_mutex_lock(&q->mutex);
    while (!q->stop) {
        while (!q->count && !q->stop && !q->drain) pthread_cond_wait(&q->changed, &q->mutex);
        if (q->stop || (!q->count && q->drain)) break;
        /* Occupied bytes remain reserved while the mutex is released. */
        size_t available = q->capacity-q->head;
        if (available > q->count) available = q->count;
        if (available > q->write_limit) available = q->write_limit;
        const unsigned char *data = q->data+q->head;
        pthread_mutex_unlock(&q->mutex);
        struct pollfd pfd = {.fd=q->fd, .events=POLLOUT};
        int ready = poll(&pfd, 1, 100), error = 0;
        ssize_t n = 0;
        int stalled=0;
        if (ready < 0 && errno != EINTR) error = errno;
        if (ready > 0) {
            n = write(q->fd, data, available);
            stalled = !n || (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK));
            if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) error = errno;
        }
        pthread_mutex_lock(&q->mutex);
        if (error) { q->error = error; q->stop = 1; }
        if (n > 0) {
            q->head = (q->head+(size_t)n)%q->capacity;
            q->count -= (size_t)n; q->written += (size_t)n;
        }
        if (stalled && !q->stop) {
            ++q->write_stalls;
            /* Windows pipe POLLOUT is advisory: a full NOWAIT pipe still
             * returns EAGAIN. Release the mutex and wait instead of spinning.
             * Producers keep receiving; Stop interrupts the wait immediately. */
            struct timespec until;clock_gettime(CLOCK_REALTIME,&until);
            until.tv_nsec+=1000000;
            if (until.tv_nsec>=1000000000) {++until.tv_sec;until.tv_nsec-=1000000000;}
            while (!q->stop) {
                int rc=pthread_cond_timedwait(&q->changed,&q->mutex,&until);
                if (rc) break;
            }
        }
    }
    pthread_mutex_unlock(&q->mutex);
    return NULL;
}

IQOutput *iq_output_create(int fd, size_t block_bytes, size_t slots) {
    if (!block_bytes || !slots || slots > SIZE_MAX/block_bytes) { errno = EINVAL; return NULL; }
    IQOutput *q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->fd = fd; q->capacity = block_bytes*slots;
    q->write_limit=q->capacity;
#ifdef _WIN32
    /* Query once, not per RX block. A large backlog must not become one
     * write larger than the pipe's entire buffer: NOWAIT pipes under Wine
     * can reject that request even when empty. Keep ordinary blocks batched
     * up to the actual pipe capacity and still account for partial writes. */
    HANDLE handle=(HANDLE)_get_osfhandle(fd);
    DWORD pipe_bytes=0;
    if (GetFileType(handle)==FILE_TYPE_PIPE &&
        GetNamedPipeInfo(handle,NULL,&pipe_bytes,NULL,NULL) && pipe_bytes &&
        pipe_bytes<q->write_limit) q->write_limit=pipe_bytes;
#endif
    q->data = malloc(q->capacity);
    if (!q->data) goto fail;
    memset(q->data, 0, q->capacity);
    int err = pthread_mutex_init(&q->mutex, NULL);
    if (err) { errno = err; goto fail; }
    err = pthread_cond_init(&q->changed, NULL);
    if (err) { pthread_mutex_destroy(&q->mutex); errno = err; goto fail; }
    q->flags = fcntl(fd, F_GETFL);
    if (q->flags < 0 || fcntl(fd, F_SETFL, q->flags | O_NONBLOCK) < 0) goto sync_fail;
    err = pthread_create(&q->thread, NULL, writer, q);
    if (!err) return q;
    fcntl(fd, F_SETFL, q->flags); errno = err;
sync_fail:
    pthread_cond_destroy(&q->changed); pthread_mutex_destroy(&q->mutex);
fail:
    free(q->data); free(q); return NULL;
}

int iq_output_push(IQOutput *q, const void *data, size_t bytes) {
    pthread_mutex_lock(&q->mutex);
    int result = 0;
    if (q->stop || q->error || bytes > q->capacity) result = -1;
    else if (bytes > q->capacity-q->count) { q->dropped += bytes; result = 1; }
    else if (bytes) {
        size_t tail = (q->head+q->count)%q->capacity;
        size_t first = q->capacity-tail;
        if (first > bytes) first = bytes;
        memcpy(q->data+tail, data, first);
        memcpy(q->data, (const unsigned char *)data+first, bytes-first);
        int was_empty=!q->count;
        q->count += bytes; q->accepted += bytes;
        if (q->count > q->peak) q->peak = q->count;
        /* Only an empty queue can have a writer waiting for producer data. */
        if (was_empty) pthread_cond_signal(&q->changed);
    }
    pthread_mutex_unlock(&q->mutex); return result;
}

void iq_output_stats(IQOutput *q, IQOutputStats *stats) {
    pthread_mutex_lock(&q->mutex);
    *stats = (IQOutputStats){.accepted=q->accepted, .written=q->written, .dropped=q->dropped,
        .write_stalls=q->write_stalls,
        .queued=q->count, .capacity=q->capacity, .peak=q->peak};
    pthread_mutex_unlock(&q->mutex);
}

int iq_output_error(IQOutput *q) {
    pthread_mutex_lock(&q->mutex); int error = q->error;
    pthread_mutex_unlock(&q->mutex); return error;
}

static int finish(IQOutput *q, int drain) {
    if (!q) return 0;
    pthread_mutex_lock(&q->mutex);
    if (drain) q->drain = 1; else q->stop = 1;
    pthread_cond_signal(&q->changed); pthread_mutex_unlock(&q->mutex);
    pthread_join(q->thread, NULL);
    fcntl(q->fd, F_SETFL, q->flags);
    pthread_cond_destroy(&q->changed); pthread_mutex_destroy(&q->mutex);
    int error = q->error;
    free(q->data); free(q);
    return error;
}

void iq_output_destroy(IQOutput *q) { (void)finish(q, 0); }
int iq_output_finish(IQOutput *q) { return finish(q, 1); }
