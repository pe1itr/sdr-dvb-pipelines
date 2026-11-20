#define _POSIX_C_SOURCE 200809L
#include "scatter_backend.h"
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    g_assert_true(argc==3 || argc==4); signal(SIGPIPE,SIG_IGN);
    char *bytes; gsize size;
    g_assert_true(g_file_get_contents(argv[1],&bytes,&size,NULL));
    int input[2]; g_assert_cmpint(pipe(input),==,0);
    int output=open(argv[2],O_CREAT|O_TRUNC|O_WRONLY,0600); g_assert_cmpint(output,>=,0);
    GError *error=NULL;
    DsmccOutput *data_out=NULL;
    if (argc==4) {
        data_out=dsmcc_output_open("127.0.0.1",g_ascii_strtoll(argv[3],NULL,10),&error);
        g_assert_no_error(error); g_assert_nonnull(data_out);
    }
    ScatterBackend *s=scatter_backend_start(input[0],output,FALSE,data_out,&error);
    g_assert_no_error(error); g_assert_nonnull(s); close(input[0]); close(output);
    int rounds=data_out ? 1 : 3;
    for (int round=0;round<rounds;round++) {
        scatter_backend_set_enabled(s,round%2);
        for (gsize at=0;at<size;) {
            ssize_t n=write(input[1],bytes+at,MIN(size-at,137)); g_assert_cmpint(n,>,0); at+=n;
        }
        if (data_out) continue;
        gint64 deadline=g_get_monotonic_time()+3000000;
        gboolean ready=FALSE;
        do {
            TsInfoSnapshot snapshot={0}; ts_info_backend_snapshot(s->information,&snapshot);
            ready=snapshot.page && snapshot.events[0].valid;
            ts_info_snapshot_clear(&snapshot);
            if (!ready) g_usleep(1000);
        } while (!ready && g_get_monotonic_time()<deadline);
        g_assert_true(ready);
        ts_info_backend_reset(s->information);
        TsInfoSnapshot cleared={0}; ts_info_backend_snapshot(s->information,&cleared);
        g_assert_null(cleared.page); g_assert_cmpuint(cleared.count,==,0);
        g_assert_false(cleared.events[0].valid); ts_info_snapshot_clear(&cleared);
    }
    close(input[1]);
    gint64 deadline=g_get_monotonic_time()+3000000;
    while (!scatter_backend_done(s) && g_get_monotonic_time()<deadline) g_usleep(1000);
    g_assert_true(scatter_backend_done(s)); g_assert_false(g_atomic_int_get(&s->relay_failed));
    scatter_backend_destroy(s);
    char *result; gsize received;
    g_assert_true(g_file_get_contents(argv[2],&result,&received,NULL));
    g_assert_cmpuint(received,==,size*rounds);
    for (int i=0;i<rounds;i++) g_assert_cmpmem(bytes,size,result+size*i,size);
    g_free(result); g_free(bytes);

    /* An oversized metadata chunk and a full queue are discarded, not blocked.
     * Reset must hide every snapshot from work already in flight. */
    TsInfoBackend *b=ts_info_backend_new();
    guint8 *noise=g_malloc0(TS_INFO_QUEUE_LIMIT+1);
    ts_info_backend_feed(b,noise,TS_INFO_QUEUE_LIMIT+1);
    for (unsigned i=0;i<64;i++) ts_info_backend_feed(b,noise,65536);
    ts_info_backend_reset(b); ts_info_backend_stop(b);
    deadline=g_get_monotonic_time()+1000000;
    while (!ts_info_backend_done(b) && g_get_monotonic_time()<deadline) g_usleep(1000);
    g_assert_true(ts_info_backend_done(b)); ts_info_backend_free(b); g_free(noise);
    return 0;
}
