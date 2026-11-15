#define _POSIX_C_SOURCE 200809L
#include "dvbt_bonding.h"
#include "dvb_bond.h"
#include "iq_shift.h"
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* Reuse the multirx decoder's --frequency-offset/--channel-filter path.
 * Every lane has its own bounded IQ queue and decoder process. Only this
 * coordinator touches the merger; TS readers never call it concurrently. */
typedef struct { size_t size; unsigned char bytes[]; } Chunk;
typedef struct { unsigned slot; size_t size; unsigned char bytes[188]; } Packet;
typedef struct Host Host;
typedef struct {
    Host *host; unsigned slot; GSubprocess *process;
    GAsyncQueue *iq; GThread *writer, *reader;
    size_t queued; int started; guint64 drops;
} Lane;
struct Host {
    Lane lanes[2]; GAsyncQueue *ts; GMutex lock;
    int eof, failed; size_t queue_limit; unsigned sample_bytes;
};
static uint64_t now_ns(void) { return (uint64_t)g_get_monotonic_time()*1000; }
static void fail(Host *h, const char *reason) {
    if (g_atomic_int_compare_and_exchange(&h->failed,0,1)) fprintf(stderr,"[bond] error: %s\n",reason);
}
static Chunk *chunk_new(const void *bytes, size_t size) {
    Chunk *c=g_malloc(sizeof(*c)+size); c->size=size;
    if (size) memcpy(c->bytes,bytes,size);
    return c;
}
static gpointer write_lane(gpointer data) {
    Lane *l=data; Host *h=l->host;
    GOutputStream *out=g_subprocess_get_stdin_pipe(l->process);
    for (;;) {
        Chunk *c=g_async_queue_pop(l->iq);
        g_mutex_lock(&h->lock); l->queued-=c->size; g_mutex_unlock(&h->lock);
        if (!c->size) {g_free(c);break;}
        gboolean ok=g_output_stream_write_all(out,c->bytes,c->size,NULL,NULL,NULL);g_free(c);
        if (!ok) {fail(h,"decoder IQ pipe closed");break;}
    }
    g_output_stream_close(out,NULL,NULL);return NULL;
}
static gpointer read_lane(gpointer data) {
    Lane *l=data; Host *h=l->host;
    GInputStream *in=g_subprocess_get_stdout_pipe(l->process);
    for (;;) {
        Packet *p=g_new0(Packet,1);p->slot=l->slot;
        gboolean ok=g_input_stream_read_all(in,p->bytes,188,&p->size,NULL,NULL);
        if (!ok || (p->size && p->size!=188)) {fail(h,"incomplete decoder TS packet");g_free(p);break;}
        if (g_async_queue_length(h->ts)>=8192) {fail(h,"TS queue capacity exceeded");g_free(p);break;}
        size_t n=p->size;g_async_queue_push(h->ts,p);if (!n) break;
    }
    return NULL;
}
static gpointer read_iq(gpointer data) {
    Host *h=data; unsigned char bytes[16384];
    for (;;) {
        size_t n=fread(bytes,h->sample_bytes,sizeof(bytes)/h->sample_bytes,stdin)*h->sample_bytes;
        g_mutex_lock(&h->lock);
        for (unsigned i=0;i<2;++i) {
            Lane *l=&h->lanes[i];if (!l->started) continue;
            if (n && l->queued+n>h->queue_limit) {l->drops+=n/h->sample_bytes;continue;}
            l->queued+=n;g_async_queue_push(l->iq,chunk_new(bytes,n));
        }
        if (!n) h->eof=1;
        g_mutex_unlock(&h->lock);
        if (!n) {if (ferror(stdin)) fail(h,"IQ read failed");break;}
    }
    return NULL;
}
static gboolean start_lane(Host *h,unsigned slot,char **decoder,double offset) {
    Lane *l=&h->lanes[slot];
    GPtrArray *args=g_ptr_array_new();
    for (unsigned i=0;decoder[i];++i) g_ptr_array_add(args,decoder[i]);
    char value[G_ASCII_DTOSTR_BUF_SIZE];g_ascii_dtostr(value,sizeof(value),offset);
    g_ptr_array_add(args,"--frequency-offset");g_ptr_array_add(args,value);
    g_ptr_array_add(args,"--raw-ts-output");
    g_ptr_array_add(args,"--channel-filter");g_ptr_array_add(args,NULL);
    GSubprocessLauncher *launcher=g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE|G_SUBPROCESS_FLAGS_STDOUT_PIPE);
    if (slot) g_subprocess_launcher_unsetenv(launcher,"LINRAD_DECODER_MONITOR_PORT");
    GError *error=NULL;
    l->process=g_subprocess_launcher_spawnv(launcher,(const gchar *const *)args->pdata,&error);
    g_object_unref(launcher);g_ptr_array_free(args,TRUE);
    if (!l->process) {fail(h,error->message);g_clear_error(&error);return FALSE;}
    l->writer=g_thread_new("bond-iq",write_lane,l);l->reader=g_thread_new("bond-ts",read_lane,l);
    g_mutex_lock(&h->lock);l->started=1;
    if (h->eof) g_async_queue_push(l->iq,chunk_new(NULL,0));
    g_mutex_unlock(&h->lock);
    fprintf(stderr,"[bond] receiver=%u offset=%+.0f Hz\n",slot,offset);return TRUE;
}
int dvbt_bonding_main(int argc,char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
#endif
    /* Host-only arguments precede --; everything after it belongs to the
     * exact same DVB-T worker used by the headless multirx repeater. */
    double offset=0,rate=0;int automatic=1,sep=0;
    for (int i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--")) {sep=i;break;}
        if (!strcmp(argv[i],"--auto")) automatic=1;
        else if (!strcmp(argv[i],"--no-auto")) automatic=0;
        else if (!strcmp(argv[i],"--anchor-offset") && i+1<argc) offset=g_ascii_strtod(argv[++i],NULL);
        else return 2;
    }
    if (!sep || sep+1>=argc) return 2;
    char **decoder=argv+sep+1;
    const char *format="s16";
    for (int i=0;decoder[i];++i) {
        if (!strcmp(decoder[i],"--sample-rate") && decoder[i+1]) rate=g_ascii_strtod(decoder[i+1],NULL);
        if (!strcmp(decoder[i],"--input-format") && decoder[i+1]) format=decoder[i+1];
    }
    if (!iq_shift_fits(rate,offset,40000) || (strcmp(format,"s16") && strcmp(format,"u8"))) return 2;
    Host h={0};g_mutex_init(&h.lock);h.ts=g_async_queue_new_full(g_free);
    h.sample_bytes=!strcmp(format,"u8") ? 2 : 4;
    h.queue_limit=(size_t)(rate*h.sample_bytes/2);
    for (unsigned i=0;i<2;++i) {h.lanes[i].host=&h;h.lanes[i].slot=i;h.lanes[i].iq=g_async_queue_new_full(g_free);}
    dvb_bond_config_v1 config;dvb_bond_config_init_v1(&config);config.session=0;
    dvb_bond_rx *rx=dvb_bond_rx_create_v1(&config);
    if (!rx || !start_lane(&h,0,decoder,offset)) return 1;
    GThread *input=g_thread_new("bond-source",read_iq,&h);
    const char *control=g_getenv("LINRAD_BOND_CONTROL");
    int discovered=0,active=0,stream_end=0,ended[2]={0,1};double peer_offset=0;
    uint64_t last_packet=now_ns(),last_report=0;
    while (!g_atomic_int_get(&h.failed)) {
        uint64_t now=now_ns();
        if (discovered && !active && !automatic && control && g_file_test(control,G_FILE_TEST_EXISTS)) {
            /* Learn a fresh anchor before starting slot 1. Resetting after
             * starting it could let the partner become the new anchor and
             * invert the requested relative tuning record. */
            dvb_bond_rx_reset(rx);discovered=0;automatic=1;
            fprintf(stderr,"[bond] waiting for fresh anchor\n");
        }
        if (discovered && !active && automatic) {
            /* Discovery offsets are nominal relative separations. Never tune
             * recorded IQ to the announcement's absolute transmitter RF. */
            if (!iq_shift_fits(rate,peer_offset,40000)) {fail(&h,"partner outside shared IQ passband");break;}
            if (!start_lane(&h,1,decoder,peer_offset)) break;
            ended[1]=0;active=1;fprintf(stderr,"[bond] active\n");
        }
        Packet *p=g_async_queue_timeout_pop(h.ts,1000);
        if (p) {
            if (!p->size) ended[p->slot]=1;
            else {
                last_packet=now;
                if (dvb_bond_rx_push(rx,p->slot,p->bytes,now)<0) {
                    /* A malformed/lost outer packet is receiver damage;
                     * library counters and deadlines handle recovery. */
                }
                /* Detection alone must not mute standalone reception when
                 * automatic bonding is disabled. Switch output only once
                 * the partner and merger are actually enabled. */
                if (!active && fwrite(p->bytes,1,188,stdout)!=188) fail(&h,"TS output closed");
            }
            g_free(p);
        }
        dvb_bond_event_v1 event;
        while (dvb_bond_rx_poll_event(rx,&event)==1) {
            if (event.type==DVB_BOND_EVENT_TUNE_PARTNER) {
                discovered=1;peer_offset=offset+(double)event.partner_offset_hz;
                fprintf(stderr,"[bond] detected offset=%+.0f partner_hz=%llu\n",peer_offset,(unsigned long long)event.frequency_hz);
            } else if (event.type==DVB_BOND_EVENT_PAIR_CONFIRMED) fprintf(stderr,"[bond] pair confirmed\n");
            else if (event.type==DVB_BOND_EVENT_RESTART) {
                stream_end=0;
                fprintf(stderr,"[bond] stream restarted\n");
            }
        }
        dvb_bond_status_v1 state;dvb_bond_rx_status(rx,&state);
        if (active && ended[0] && ended[1] && !state.ended &&
            state.distribution==DVB_BOND_DISTRIBUTION_SEPARATE_LANES) {
            if (dvb_bond_rx_finish(rx)<0) {fail(&h,"native merger finish failed");break;}
        }
        if (active && !stream_end) {
            uint8_t ts[188];int rc;
            for (unsigned n=0;n<128;++n) {
                rc=dvb_bond_rx_pop(rx,now,ts);
                if (rc==DVB_BOND_END) {stream_end=1;break;}
                if (rc!=DVB_BOND_PACKET) break;
                if (fwrite(ts,1,188,stdout)!=188) {fail(&h,"TS output closed");break;}
            }
        }
        if (fflush(stdout)) {fail(&h,"TS output closed");break;}
        if (discovered && now-last_report>=UINT64_C(1000000000)) {
            char json[4096];if (dvb_bond_rx_status_json(rx,json,sizeof(json))>=0) fprintf(stderr,"[bond-status] %s\n",json);
            last_report=now;
        }
        dvb_bond_rx_status(rx,&state);
        if (ended[0] && ended[1] && (!active || stream_end ||
            (!state.ended && now-last_packet>config.playout_delay_ns+UINT64_C(2000000000)))) break;
    }
    /* On errors a blocked source read need not finish. Process exit closes it;
     * force-exit children before returning so failed lanes cannot linger. */
    if (g_atomic_int_get(&h.failed)) {
        for (unsigned i=0;i<2;++i) if (h.lanes[i].process) g_subprocess_force_exit(h.lanes[i].process);
        return 1;
    }
    g_thread_join(input);
    for (unsigned i=0;i<2;++i) {
        Lane *l=&h.lanes[i];if (!l->started) {g_async_queue_unref(l->iq);continue;}
        g_thread_join(l->writer);g_thread_join(l->reader);
        if (!g_subprocess_wait_check(l->process,NULL,NULL)) h.failed=1;
        fprintf(stderr,"[bond] receiver=%u iq_dropped=%llu\n",i,(unsigned long long)l->drops);
        g_object_unref(l->process);g_async_queue_unref(l->iq);
    }
    char final_status[4096];
    if (discovered && dvb_bond_rx_status_json(rx,final_status,sizeof(final_status))>=0)
        fprintf(stderr,"[bond-status] %s\n",final_status);
    dvb_bond_rx_destroy(rx);g_async_queue_unref(h.ts);g_mutex_clear(&h.lock);
    return h.failed ? 1 : 0;
}
