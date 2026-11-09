#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Reproduce Windows pipe readiness on both platforms: writable is advisory
 * even when the actual byte pipe is full. Keep real writes/partial writes. */
static atomic_uint attempts;
static int always_writable(struct pollfd *p, nfds_t n, int timeout) {
    (void)timeout; assert(n==1);p->revents=POLLOUT;
    atomic_fetch_add(&attempts,1);return 1;
}
#undef poll
#define poll always_writable
#include "../src/iq_output.c"
#undef poll

static double now(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec+t.tv_nsec/1e9;
}
static int byte_pipe(int fds[2]) {
#ifdef _WIN32
    return pipe2(fds,0);
#else
    return pipe(fds);
#endif
}

int main(void) {
    signal(SIGPIPE,SIG_IGN);
    int fds[2];assert(!byte_pipe(fds));
    assert(!fcntl(fds[1],F_SETFL,O_NONBLOCK));
    unsigned char filler[4096]={0}, received[4096], payload[65536];
    for (size_t i=0;i<sizeof(payload);++i) payload[i]=(unsigned char)(i*37+11);
    size_t filled=0;
    for (;;) {
        ssize_t n=write(fds[1],filler,sizeof(filler));
        if (n<0) {assert(errno==EAGAIN || errno==EWOULDBLOCK);break;}
        assert(n>0);filled+=(size_t)n;
    }
    IQOutput *q=iq_output_create(fds[1],sizeof(payload),4);assert(q);
    assert(!iq_output_push(q,payload,sizeof(payload)));
    double start=now();
    struct timespec delay={.tv_nsec=200000000};nanosleep(&delay,NULL);
    unsigned retries=atomic_load(&attempts);
    IQOutputStats stats;iq_output_stats(q,&stats);
    assert(stats.write_stalls>0);
    assert(stats.written==0 && stats.queued==sizeof(payload));
    assert(retries>0 && retries < (now()-start)*2000+20);
    printf("Full pipe: %u write attempts in %.3f s\n",retries,now()-start);

    /* Resume a fragmented consumer. Filler and IQ must arrive exactly once,
     * in order, despite partial writes and repeated backpressure. */
    size_t total=0;
    while (total<filled+sizeof(payload)) {
        ssize_t n=read(fds[0],received,997);assert(n>0);
        for (ssize_t j=0;j<n;++j) {
            size_t at=total+(size_t)j;
            assert(at<filled+sizeof(payload));
            assert(received[j]==(at<filled ? 0 : payload[at-filled]));
        }
        total+=(size_t)n;
    }
    assert(!iq_output_finish(q));
    close(fds[0]);close(fds[1]);

    /* Cancel while backpressured without waiting for the reader. */
    assert(!byte_pipe(fds));
    q=iq_output_create(fds[1],sizeof(payload),64);assert(q);
    for (int i=0;i<64;++i) assert(!iq_output_push(q,payload,sizeof(payload)));
    nanosleep(&delay,NULL);
    start=now();iq_output_destroy(q);assert(now()-start<1);
    close(fds[0]);close(fds[1]);
    puts("Backpressure: bounded retries, exact bytes, resumed reads and prompt stop passed");
    return 0;
}
