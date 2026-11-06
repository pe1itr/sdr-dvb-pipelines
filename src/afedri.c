#define _GNU_SOURCE
#include "afedri.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Wire commands follow the AFEDRI protocol and Linrad afedri.c.
 * TCP replies are framed: reads must tolerate fragmentation/coalescing. */
enum { PACKET_BYTES = 1028, PAYLOAD_BYTES = 1024, RX_BATCH = 32 };
struct Afedri {
    int tcp, udp, started, have_seq, receiving, thread_started, sync_ready;
    struct in_addr address;
    uint16_t expected;
    uint64_t received, lost, stale, invalid, socket_drops, queue_drops, filled;
    uint32_t last_socket_drops;
    double last_iq, rate;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    atomic_int stop;
    int error;
    unsigned char *queue;
    size_t capacity, head, count, peak;
};
static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static int wait_fd(int fd, short events, double deadline) {
    struct pollfd p = {.fd=fd, .events=events};
    double remaining = deadline - now();
    if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
    int n = poll(&p, 1, (int)ceil(remaining * 1000));
    if (!n) errno = ETIMEDOUT;
    return n > 0 ? 0 : -1;
}
static int transfer(int fd, unsigned char *p, size_t n, int writing, double deadline) {
    while (n) {
        if (wait_fd(fd, writing ? POLLOUT : POLLIN, deadline)) return -1;
        ssize_t count = writing ? send(fd, p, n, MSG_NOSIGNAL) : recv(fd, p, n, 0);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) { if (!count) errno = ECONNRESET; return -1; }
        p += count; n -= count;
    }
    return 0;
}
static int settle(long nanoseconds) {
    struct timespec delay = {.tv_nsec=nanoseconds};
    return nanosleep(&delay, NULL);
}
static int exchange(Afedri *s, const unsigned char *cmd, unsigned char *reply, size_t expected) {
    double deadline = now() + 1.5;
    if (transfer(s->tcp, (unsigned char *)cmd, cmd[0], 1, deadline) ||
        settle(100000000) || /* Preserve Python xchg's firmware settling time. */
        transfer(s->tcp, reply, 2, 0, deadline)) goto error;
    size_t length = reply[0] | ((reply[1] & 0x1f) << 8);
    if (length < 4 || length > 256) { errno = EPROTO; goto error; }
    if (transfer(s->tcp, reply + 2, length - 2, 0, deadline)) goto error;
    if (length < expected || (reply[1] & 0xe0) != (cmd[1] == 0xe0 ? 0xe0 : 0) ||
        (cmd[1] != 0xe0 && (reply[2] != cmd[2] || reply[3] != cmd[3])) ||
        (cmd[1] == 0xe0 && (reply[2] != 6 ||
            (cmd[3] == 48 ? (reply[3] != 28 || reply[4] != 48) : reply[3] != cmd[3])))) {
        errno = EPROTO; goto error;
    }
    return (int)length;
error:
    fprintf(stderr, "[AFEDRI] Command %02x/%02x failed: %s\n", cmd[2], cmd[3], strerror(errno));
    return -1;
}
static int set(Afedri *s, const unsigned char *cmd) {
    unsigned char reply[256];
    int n = exchange(s, cmd, reply, cmd[0]);
    if (n < 0) return -1;
    if (cmd[1] != 0xe0 && memcmp(cmd + 4, reply + 4, cmd[2] == 0x18 ? 3 : cmd[0] - 4)) {
        fprintf(stderr, "[AFEDRI] Setting %02x rejected or changed by receiver\n", cmd[2]);
        return -1;
    }
    return 0;
}
static void le32(unsigned char *p, uint32_t value) {
    for (int i=0; i<4; ++i) p[i] = value >> (8*i);
}
int afedri_frequency(Afedri *s, double hz) {
    unsigned char cmd[] = {10, 0, 0x20, 0, 0, 0, 0, 0, 0, 0};
    le32(cmd + 5, (uint32_t)hz); return set(s, cmd);
}
int afedri_gain(Afedri *s, double db) {
    unsigned char cmd[] = {6, 0, 0x38, 0, 0, 8 * (((int)db + 10) / 3) + 4};
    return set(s, cmd);
}
static int receiver_state(Afedri *s, int run) {
    unsigned char cmd[] = {8, 0, 0x18, 0, 0x80, run ? 2 : 1, 0, 1};
    return set(s, cmd);
}
Afedri *afedri_open(const char *ip, int port, const char *bind_ip,
                    double rate, double frequency, double gain, double *actual_rate) {
    Afedri *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->tcp = s->udp = -1;
    s->rate = rate;
    atomic_init(&s->stop, 0);
    s->capacity = (size_t)ceil(rate / 256) * PAYLOAD_BYTES; /* One second, packet aligned. */
    s->queue = calloc(1, s->capacity);
    if (!s->queue) goto fail;
    int err = pthread_mutex_init(&s->mutex, NULL);
    if (err) goto fail;
    pthread_condattr_t attr;
    err = pthread_condattr_init(&attr);
    if (err) { pthread_mutex_destroy(&s->mutex); goto fail; }
    err = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!err) err = pthread_cond_init(&s->changed, &attr);
    pthread_condattr_destroy(&attr);
    if (err) { pthread_mutex_destroy(&s->mutex); goto fail; }
    s->sync_ready = 1;
    struct sockaddr_in peer = {.sin_family=AF_INET, .sin_port=htons(port)};
    struct sockaddr_in local = {.sin_family=AF_INET, .sin_port=htons(port)};
    if (port < 1 || port > 65535 || inet_pton(AF_INET, ip, &peer.sin_addr) != 1 ||
        inet_pton(AF_INET, bind_ip, &local.sin_addr) != 1) {
        fprintf(stderr, "[AFEDRI] Invalid IPv4 address or port\n"); goto fail;
    }
    s->address = peer.sin_addr;
    fprintf(stderr, "[AFEDRI] Connecting to %s:%d\n", ip, port);
    s->tcp = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (s->tcp < 0) goto system_error;
    if (connect(s->tcp, (struct sockaddr *)&peer, sizeof(peer))) {
        if (errno != EINPROGRESS || wait_fd(s->tcp, POLLOUT, now() + 2)) goto system_error;
        int err = 0; socklen_t len = sizeof(err);
        if (getsockopt(s->tcp, SOL_SOCKET, SO_ERROR, &err, &len)) goto system_error;
        if (err) { errno = err; goto system_error; }
    }
    unsigned char reply[256];
    const unsigned char name[] = {4, 0x20, 1, 0};
    int n = exchange(s, name, reply, 5);
    if (n < 0) goto fail;
    fprintf(stderr, "[AFEDRI] Online: %.*s (%s:%d), TCP reply confirmed\n", n-4, reply+4, ip, port);
    /* EEPROM clock words are used by Linrad; do not assume 76.8 or 80 MHz. */
    uint32_t clock = 0;
    for (int word=0; word<2; ++word) {
        unsigned char cmd[] = {9, 0xe0, 2, 0x55, word, 0, 0, 0, 0};
        if (exchange(s, cmd, reply, 9) < 0) goto fail;
        clock |= ((uint32_t)reply[4] | ((uint32_t)reply[5] << 8)) << (16*word);
    }
    if (clock < 10000000 || clock > 200000000) {
        fprintf(stderr, "[AFEDRI] Invalid ADC clock: %u Hz\n", clock); goto fail;
    }
    unsigned divisor = (unsigned)(clock / rate) / 4;
    if (clock > rate * (4 * divisor + 2)) ++divisor;
    *actual_rate = floor(clock / (4.0 * divisor));
    if (fabs(*actual_rate - rate) > .5) {
        fprintf(stderr, "[AFEDRI] Requested rate %.0f, hardware rate %.0f (ADC %u Hz); select the hardware rate in Input\n",
                rate, *actual_rate, clock); goto fail;
    }
    /* Bind before start and fail rather than share a port with another receiver. */
    s->udp = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (s->udp < 0) goto system_error;
    int buffer = 4 * 1024 * 1024, overflow = 1;
    if (setsockopt(s->udp, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer)) ||
        setsockopt(s->udp, SOL_SOCKET, SO_RXQ_OVFL, &overflow, sizeof(overflow)) ||
        bind(s->udp, (struct sockaddr *)&local, sizeof(local))) goto system_error;
    socklen_t buffer_length = sizeof(buffer);
    if (getsockopt(s->udp, SOL_SOCKET, SO_RCVBUF, &buffer, &buffer_length)) goto system_error;
    fprintf(stderr, "[AFEDRI] UDP socket buffer %d bytes; independent receive queue %.0f ms\n",
        buffer, s->capacity/(rate*4)*1000);
    if (receiver_state(s, 0)) goto fail;
    const unsigned char single[] = {9, 0xe0, 2, 48, 0, 0, 0, 0, 0};
    const unsigned char packets[] = {5, 0, 0xc4, 0, 0};
    unsigned char sample_rate[] = {9, 0, 0xb8, 0, 0, 0, 0, 0, 0};
    le32(sample_rate + 5, (uint32_t)rate);
    if (set(s, single) || set(s, sample_rate) || set(s, packets) ||
        afedri_frequency(s, frequency) || afedri_gain(s, gain)) goto fail;
    return s;
system_error:
    fprintf(stderr, "[AFEDRI] Network: %s\n", strerror(errno));
fail:
    afedri_close(s); return NULL;
}
/* Only this thread reads UDP. No FFTs, TCP round trips, stdout or logging here.
 * The mutex protects queue bytes and counters, never downstream DSP or I/O. */
static int queue_packet(Afedri *s, const unsigned char *payload) {
    if (s->count + PAYLOAD_BYTES > s->capacity) { ++s->queue_drops; return 0; }
    size_t tail = (s->head + s->count) % s->capacity;
    memcpy(s->queue + tail, payload, PAYLOAD_BYTES);
    s->count += PAYLOAD_BYTES;
    if (s->count > s->peak) s->peak = s->count;
    return 1;
}
static void *receive_udp(void *opaque) {
    Afedri *s = opaque;
    unsigned char packets[RX_BATCH][PACKET_BYTES];
    union { struct cmsghdr align; unsigned char data[CMSG_SPACE(sizeof(uint32_t))]; } control[RX_BATCH];
    struct sockaddr_in from[RX_BATCH];
    struct iovec iov[RX_BATCH];
    struct mmsghdr msgs[RX_BATCH];
    static const unsigned char silence[PAYLOAD_BYTES];
    while (!atomic_load_explicit(&s->stop, memory_order_relaxed)) {
        struct pollfd fd = {.fd=s->udp, .events=POLLIN};
        int n = poll(&fd, 1, 20);
        if (n < 0 && errno == EINTR) continue;
        if (!n) continue;
        if (n < 0 || (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            pthread_mutex_lock(&s->mutex);
            s->error = n < 0 ? errno : EIO;
            pthread_cond_signal(&s->changed); pthread_mutex_unlock(&s->mutex); break;
        }
        memset(msgs, 0, sizeof(msgs));
        for (int i=0; i<RX_BATCH; ++i) {
            iov[i] = (struct iovec){.iov_base=packets[i], .iov_len=PACKET_BYTES};
            msgs[i].msg_hdr = (struct msghdr){.msg_name=&from[i], .msg_namelen=sizeof(from[i]),
                .msg_iov=&iov[i], .msg_iovlen=1, .msg_control=control[i].data, .msg_controllen=sizeof(control[i].data)};
        }
        n = recvmmsg(s->udp, msgs, RX_BATCH, MSG_DONTWAIT, NULL);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        pthread_mutex_lock(&s->mutex);
        if (n < 0) s->error = errno;
        for (int i=0; i<n; ++i) {
            for (struct cmsghdr *c=CMSG_FIRSTHDR(&msgs[i].msg_hdr); c; c=CMSG_NXTHDR(&msgs[i].msg_hdr,c)) {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL && c->cmsg_len >= CMSG_LEN(sizeof(uint32_t))) {
                    uint32_t drops; memcpy(&drops, CMSG_DATA(c), sizeof(drops));
                    s->socket_drops += (uint32_t)(drops - s->last_socket_drops);
                    s->last_socket_drops = drops;
                }
            }
            unsigned char *packet = packets[i];
            if (msgs[i].msg_len != PACKET_BYTES || (msgs[i].msg_hdr.msg_flags & MSG_TRUNC) ||
                from[i].sin_addr.s_addr != s->address.s_addr || packet[0] != 4 || packet[1] != 0x84) {
                ++s->invalid; continue;
            }
            uint16_t seq = packet[2] | ((uint16_t)packet[3] << 8);
            if (s->have_seq) {
                uint16_t gap = seq - s->expected;
                if (gap >= 32768) { ++s->stale; continue; }
                s->lost += gap;
                /* Like Linrad: retain the sample clock across small real gaps.
                 * Still count/report loss; never hide it or expand a large jump. */
                if (gap < 25) for (unsigned j=0; j<gap; ++j) {
                    s->filled += queue_packet(s, silence);
                }
            }
            s->have_seq = 1; s->expected = seq + 1; s->last_iq = now(); ++s->received;
            queue_packet(s, packet + 4);
        }
        pthread_cond_signal(&s->changed); pthread_mutex_unlock(&s->mutex);
        if (n < 0) break;
    }
    return NULL;
}
int afedri_start(Afedri *s) {
    if (settle(200000000)) return -1; /* Python start_stream guard time. */
    s->last_iq = now();
    int err = pthread_create(&s->thread, NULL, receive_udp, s);
    if (err) { fprintf(stderr, "[AFEDRI] UDP receiver thread: %s\n", strerror(err)); return -1; }
    s->thread_started = 1;
    /* Receiver already draining UDP while START settles/awaits its reply. */
    s->started = 1;
    if (receiver_state(s, 1)) return -1;
    fprintf(stderr, "[AFEDRI] Stream start confirmed; waiting for UDP IQ\n");
    return 0;
}
void afedri_report(Afedri *s) {
    pthread_mutex_lock(&s->mutex);
    uint64_t received=s->received, lost=s->lost, stale=s->stale, invalid=s->invalid,
        socket_drops=s->socket_drops, queue_drops=s->queue_drops, filled=s->filled;
    size_t count=s->count, peak=s->peak, capacity=s->capacity;
    pthread_mutex_unlock(&s->mutex);
    fprintf(stderr, "[AFEDRI] UDP totals: lost=%llu stale=%llu invalid=%llu; received=%llu socket-drops=%llu input-queue-drops=%llu zero-filled=%llu; queued %.1f/%.1f ms (peak %.1f ms)\n",
        (unsigned long long)lost, (unsigned long long)stale, (unsigned long long)invalid,
        (unsigned long long)received, (unsigned long long)socket_drops,
        (unsigned long long)queue_drops, (unsigned long long)filled,
        count/(s->rate*4)*1000, capacity/(s->rate*4)*1000, peak/(s->rate*4)*1000);
}
int afedri_read(Afedri *s, int16_t *iq, size_t capacity) {
    if (!capacity || capacity % 256 || capacity > 65536) { errno = EINVAL; return -1; }
    struct pollfd fd = {.fd=s->tcp, .events=POLLIN};
    int ready = poll(&fd, 1, 0);
    if (ready < 0) return errno == EINTR ? 0 : -1;
    if (ready) {
        fprintf(stderr, "[AFEDRI] Control connection closed or unexpected TCP data\n"); return -1;
    }
    struct timespec deadline; clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += 10000000;
    if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&s->mutex);
    while (s->count < capacity*4 && !s->error) {
        int err = pthread_cond_timedwait(&s->changed, &s->mutex, &deadline);
        if (err) break;
    }
    int error = s->error, timed_out = now() - s->last_iq >= 3;
    size_t bytes = s->count < capacity*4 ? s->count : capacity*4;
    if (bytes) {
        unsigned char *out = (unsigned char *)iq;
        size_t first = s->capacity - s->head;
        if (first > bytes) first = bytes;
        memcpy(out, s->queue+s->head, first); memcpy(out+first, s->queue, bytes-first);
        s->head = (s->head+bytes)%s->capacity; s->count -= bytes;
    }
    pthread_mutex_unlock(&s->mutex);
    if (error || timed_out) {
        fprintf(stderr, "[AFEDRI] %s\n", error ? strerror(error) : "No valid UDP IQ for 3 seconds; check destination IP/port and network");
        return -1;
    }
    if (bytes && !s->receiving) {
        fprintf(stderr, "[AFEDRI] Receiving IQ: single channel, s16, UDP confirmed\n"); s->receiving = 1;
    }
    /* Convert the wire LE words to the native int16_t representation. */
    unsigned char *raw = (unsigned char *)iq;
    for (size_t i=0; i<bytes/2; ++i) iq[i] = (int16_t)((unsigned)raw[2*i] | ((unsigned)raw[2*i+1] << 8));
    return bytes/4;
}
void afedri_close(Afedri *s) {
    if (!s) return;
    if (s->started && !receiver_state(s, 0)) fprintf(stderr, "[AFEDRI] Stream stop confirmed\n");
    if (s->thread_started) {
        atomic_store_explicit(&s->stop, 1, memory_order_relaxed);
        pthread_join(s->thread, NULL);
        afedri_report(s);
    }
    if (s->udp >= 0) close(s->udp);
    if (s->tcp >= 0) close(s->tcp);
    if (s->sync_ready) { pthread_cond_destroy(&s->changed); pthread_mutex_destroy(&s->mutex); }
    free(s->queue); free(s);
}
