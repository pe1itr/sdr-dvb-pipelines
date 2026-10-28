#define _POSIX_C_SOURCE 200809L
#include "../src/iq_output.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec/1e9;
}
int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int pipefd[2]; assert(!pipe(pipefd));
    unsigned char data[262144], received[4096];
    IQOutput *q = iq_output_create(pipefd[1], sizeof(data), 8); assert(q);
    /* No consumer yet: RX can enqueue two MiB without blocking on the pipe. */
    for (int i = 0; i < 8; ++i) {
        memset(data, i, sizeof(data));
        /* UHD can return fragments: they must consume bytes, not whole slots. */
        for (size_t offset = 0; offset < sizeof(data); offset += 4096)
            assert(iq_output_push(q, data+offset, 4096) == 0);
    }
    for (int i = 0; i < 100; ++i)
        assert(iq_output_push(q, data, sizeof(data)) == 1);
    IQOutputStats overloaded; iq_output_stats(q, &overloaded);
    assert(overloaded.dropped == 100*sizeof(data));
    assert(!iq_output_error(q)); /* Saturation must not poison subsequent RX. */
    /* Resume decoder reads; verify ordering and every byte across partial writes. */
    size_t total = 0;
    while (total < 8*sizeof(data)) {
        struct pollfd pfd = {.fd=pipefd[0], .events=POLLIN};
        assert(poll(&pfd, 1, 2000) == 1);
        ssize_t n = read(pipefd[0], received, sizeof(received)); assert(n > 0);
        for (ssize_t i = 0; i < n; ++i) assert(received[i] == (total+(size_t)i)/sizeof(data));
        total += (size_t)n;
    }
    assert(!iq_output_error(q));
    IQOutputStats stats; iq_output_stats(q, &stats);
    assert(stats.accepted == 8*sizeof(data));
    assert(stats.dropped == 100*sizeof(data));
    assert(stats.capacity == 8*sizeof(data));
    assert(stats.written + stats.queued == stats.accepted);
    /* Reuse the circular buffer across its boundary with unaligned chunks. */
    for (int pass = 0; pass < 100; ++pass) {
        size_t length = 31001;
        for (size_t i = 0; i < length; ++i) data[i] = (unsigned char)(i+pass);
        assert(!iq_output_push(q, data, length));
        size_t offset = 0;
        while (offset < length) {
            struct pollfd pfd = {.fd=pipefd[0], .events=POLLIN};
            assert(poll(&pfd, 1, 2000) == 1);
            ssize_t n = read(pipefd[0], received, sizeof(received)); assert(n > 0);
            assert(offset+(size_t)n <= length);
            assert(!memcmp(received, data+offset, (size_t)n)); offset += (size_t)n;
        }
    }
    iq_output_destroy(q); close(pipefd[0]); close(pipefd[1]);

    /* Stop must not hang with a stalled decoder and a full OS pipe. */
    assert(!pipe(pipefd)); q = iq_output_create(pipefd[1], sizeof(data), 8); assert(q);
    for (int i = 0; i < 8; ++i) assert(!iq_output_push(q, data, sizeof(data)));
    double start = now(); iq_output_destroy(q); assert(now()-start < 1);
    close(pipefd[0]); close(pipefd[1]);

    /* A closed decoder pipe must propagate its error to the receiver. */
    assert(!pipe(pipefd)); q = iq_output_create(pipefd[1], sizeof(data), 2); assert(q);
    close(pipefd[0]); assert(!iq_output_push(q, data, sizeof(data)));
    start = now();
    while (!iq_output_error(q) && now()-start < 2) {
        struct timespec delay = {.tv_nsec=1000000}; nanosleep(&delay, NULL);
    }
    assert(iq_output_error(q) == EPIPE);
    assert(iq_output_push(q, data, sizeof(data)) == -1);
    iq_output_destroy(q); close(pipefd[1]);
    puts("IQ output queue: delayed consumer, byte order, fragmented input, wraparound, statistics, stop and EPIPE passed");
    return 0;
}
