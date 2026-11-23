#define _POSIX_C_SOURCE 200809L
#include "usb_receiver.h"
#include "usb_demod.h"
#include "usb_tci.h"
#include "usb_tap.h"
#include <alsa/asoundlib.h>
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
struct UsbReceiver {
    UsbTap *tap;
    char *path, *device;
    GThread *thread;
    GMutex mutex;
    gint stop, muted;
    double center, rate, offset;
    float tci_gain;
    gboolean source_active;
    MonitorMode mode;
    unsigned generation;
    char status[512];
    UsbTci *tci;
    snd_pcm_t *pcm;
    float speaker[4800];
    size_t speaker_used;
    char speaker_error[128];
    /* Protected by mutex, including the partial datagram. No UDP backlog. */
    int pcm_udp_fd;
    struct sockaddr_in pcm_udp_address;
    char pcm_udp_host[INET_ADDRSTRLEN];
    unsigned char pcm_udp_packet[480*2];
    unsigned pcm_udp_used, pcm_udp_errors;
};
UsbReceiver *usb_receiver_new(GError **error) {
    UsbReceiver *r=g_new0(UsbReceiver,1);
    r->pcm_udp_fd=-1;
    r->tci_gain=1.f;
    int fd=g_file_open_tmp("linrad-usb-XXXXXX",&r->path,error);
    if (fd<0) { g_free(r); return NULL; }
    if (ftruncate(fd,sizeof(UsbTap))<0) goto failed;
    r->tap=mmap(NULL,sizeof(UsbTap),PROT_READ | PROT_WRITE,MAP_SHARED,fd,0);
    if (r->tap==MAP_FAILED) { r->tap=NULL; goto failed; }
    close(fd);
    memset(r->tap,0,sizeof(*r->tap)); r->tap->magic=USB_TAP_MAGIC;
    atomic_init(&r->tap->enabled,0); atomic_init(&r->tap->write_index,0);
    atomic_init(&r->tap->read_index,0); atomic_init(&r->tap->dropped,0);
    g_mutex_init(&r->mutex); return r;
failed: {
    int saved=errno; close(fd); unlink(r->path); g_free(r->path); g_free(r);
    g_set_error(error,G_FILE_ERROR,g_file_error_from_errno(saved),"USB IQ buffer: %s",g_strerror(saved)); return NULL;
}}
const char *usb_receiver_tap_path(UsbReceiver *r) { return r ? r->path : NULL; }
void usb_receiver_source(UsbReceiver *r, double center, double rate, gboolean active) {
    if (!r) return;
    g_mutex_lock(&r->mutex);
    if (!isfinite(center) || !isfinite(rate) || rate<monitor_min_rate(r->mode) || rate>100000000) active=FALSE;
    if (r->center!=center || r->rate!=rate || r->source_active!=active) ++r->generation;
    r->center=center; r->rate=rate; r->source_active=active;
    if (rate>=monitor_min_rate(r->mode) && isfinite(rate))
        r->offset=CLAMP(r->offset,-rate/2-monitor_low(r->mode),rate/2-monitor_high(r->mode));
    atomic_store_explicit(&r->tap->enabled,active && r->thread!=NULL,memory_order_relaxed);
    g_mutex_unlock(&r->mutex);
}
gboolean usb_receiver_set_mode(UsbReceiver *r, MonitorMode mode) {
    if (!r || (mode!=MONITOR_USB && mode!=MONITOR_FM && mode!=MONITOR_FM_DATA)) return FALSE;
    g_mutex_lock(&r->mutex);
    gboolean valid=isfinite(r->rate) && r->rate>=monitor_min_rate(mode) &&
        r->offset+monitor_low(mode)>=-r->rate/2 && r->offset+monitor_high(mode)<=r->rate/2;
    if (valid && r->mode!=mode) { r->mode=mode; ++r->generation; }
    g_mutex_unlock(&r->mutex); return valid;
}
MonitorMode usb_receiver_mode(UsbReceiver *r) {
    if (!r) return MONITOR_USB;
    g_mutex_lock(&r->mutex); MonitorMode mode=r->mode; g_mutex_unlock(&r->mutex); return mode;
}
gboolean usb_receiver_tune(UsbReceiver *r, double frequency) {
    if (!r || !isfinite(frequency) || frequency<0) return FALSE;
    g_mutex_lock(&r->mutex);
    double offset=frequency-r->center;
    gboolean valid=r->rate>=monitor_min_rate(r->mode) && offset+monitor_low(r->mode)>=-r->rate/2 &&
        offset+monitor_high(r->mode)<=r->rate/2;
    if (valid && offset!=r->offset) { r->offset=offset; ++r->generation; }
    g_mutex_unlock(&r->mutex); return valid;
}
double usb_receiver_frequency(UsbReceiver *r) {
    if (!r) return 0;
    g_mutex_lock(&r->mutex); double frequency=r->center+r->offset; g_mutex_unlock(&r->mutex); return frequency;
}
static double tci_tune(double frequency, void *user) {
    UsbReceiver *r=user; usb_receiver_tune(r,frequency); return usb_receiver_frequency(r);
}
void usb_receiver_set_muted(UsbReceiver *r, gboolean muted) {
    if (r) g_atomic_int_set(&r->muted,!!muted);
}
gboolean usb_receiver_muted(UsbReceiver *r) { return r && g_atomic_int_get(&r->muted); }
gboolean usb_receiver_set_pcm_udp(UsbReceiver *r, gboolean enabled, const char *host, unsigned port, GError **error) {
    if (!r) {
        g_set_error_literal(error,G_FILE_ERROR,G_FILE_ERROR_FAILED,"Audio monitor unavailable"); return FALSE;
    }
    int fd=-1;
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(port)};
    if (enabled) {
        if (!host || !port || port>65535 || inet_pton(AF_INET,host,&address.sin_addr)!=1 ||
            address.sin_addr.s_addr==htonl(INADDR_ANY) || address.sin_addr.s_addr==htonl(INADDR_BROADCAST)) {
            g_set_error_literal(error,G_FILE_ERROR,G_FILE_ERROR_INVAL,"PCM UDP: enter a destination IPv4 address and port (1–65535)");
            return FALSE;
        }
        fd=socket(AF_INET,SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,0);
        if (fd<0) {
            g_set_error(error,G_FILE_ERROR,g_file_error_from_errno(errno),"PCM UDP: %s",g_strerror(errno)); return FALSE;
        }
    }
    g_mutex_lock(&r->mutex);
    if (r->pcm_udp_fd>=0) close(r->pcm_udp_fd);
    r->pcm_udp_fd=fd; r->pcm_udp_address=address;
    g_strlcpy(r->pcm_udp_host,enabled ? host : "",sizeof(r->pcm_udp_host));
    r->pcm_udp_used=r->pcm_udp_errors=0;
    g_mutex_unlock(&r->mutex); return TRUE;
}
gboolean usb_receiver_pcm_udp_enabled(UsbReceiver *r) {
    if (!r) return FALSE;
    g_mutex_lock(&r->mutex); gboolean enabled=r->pcm_udp_fd>=0; g_mutex_unlock(&r->mutex); return enabled;
}
static void pcm_udp_reset(UsbReceiver *r) {
    g_mutex_lock(&r->mutex); r->pcm_udp_used=0; g_mutex_unlock(&r->mutex);
}
static void pcm_udp_audio(UsbReceiver *r, const float *samples, size_t count) {
    g_mutex_lock(&r->mutex);
    if (r->pcm_udp_fd>=0) for (size_t i=0;i<count;++i) {
        float sample=isfinite(samples[i]) ? CLAMP(samples[i],-1.0f,1.0f) : 0;
        int16_t value=(int16_t)lrintf(sample*32767);
        r->pcm_udp_packet[r->pcm_udp_used++]=(uint16_t)value&255;
        r->pcm_udp_packet[r->pcm_udp_used++]=(uint16_t)value>>8;
        if (r->pcm_udp_used==sizeof(r->pcm_udp_packet)) {
            /* Never wait or retry: a missing/slow consumer must not stall
             * speaker, TCI or the independent DVB pipeline. */
            ssize_t sent=sendto(r->pcm_udp_fd,r->pcm_udp_packet,sizeof(r->pcm_udp_packet),MSG_DONTWAIT | MSG_NOSIGNAL,
                (struct sockaddr *)&r->pcm_udp_address,sizeof(r->pcm_udp_address));
            if (sent!=(ssize_t)sizeof(r->pcm_udp_packet)) ++r->pcm_udp_errors;
            r->pcm_udp_used=0;
        }
    }
    g_mutex_unlock(&r->mutex);
}
static void speaker_flush(UsbReceiver *r) {
    if (!r->pcm || !r->speaker_used) return;
    snd_pcm_sframes_t n=snd_pcm_writei(r->pcm,r->speaker,r->speaker_used);
    if (n<0 && n!=-EAGAIN) {
        if (n==-EPIPE) { snd_pcm_prepare(r->pcm); r->speaker_used=0; }
        else {
            g_snprintf(r->speaker_error,sizeof(r->speaker_error),"speaker unavailable: %s",snd_strerror(n));
            snd_pcm_close(r->pcm); r->pcm=NULL; r->speaker_used=0;
        }
    } else if (n>0) {
        r->speaker_used-=n; memmove(r->speaker,r->speaker+n,r->speaker_used*sizeof(float));
    }
}
void usb_receiver_set_tci_level(UsbReceiver *r, int db) {
    if (!r) return;
    float gain=powf(10.f,CLAMP(db,-60,0)/20.f);
    g_mutex_lock(&r->mutex);
    r->tci_gain=gain;
    g_mutex_unlock(&r->mutex);
}
static void audio(const float *samples, size_t count, void *user) {
    UsbReceiver *r=user;
    g_mutex_lock(&r->mutex);
    float gain=r->tci_gain;
    g_mutex_unlock(&r->mutex);
    usb_tci_audio(r->tci,samples,count,gain);
    pcm_udp_audio(r,samples,count);
    if (r->pcm && !usb_receiver_muted(r)) {
        speaker_flush(r);
        size_t n=MIN(count,G_N_ELEMENTS(r->speaker)-r->speaker_used);
        memcpy(r->speaker+r->speaker_used,samples,n*sizeof(float)); r->speaker_used+=n;
        speaker_flush(r);
    }
}
static gpointer receive(gpointer user) {
    UsbReceiver *r=user;
    int error=snd_pcm_open(&r->pcm,r->device,SND_PCM_STREAM_PLAYBACK,SND_PCM_NONBLOCK);
    if (error>=0) error=snd_pcm_set_params(r->pcm,SND_PCM_FORMAT_FLOAT,SND_PCM_ACCESS_RW_INTERLEAVED,1,48000,1,60000);
    if (error<0) {
        g_snprintf(r->speaker_error,sizeof(r->speaker_error),"speaker unavailable: %s",snd_strerror(error));
        if (r->pcm) snd_pcm_close(r->pcm);
        r->pcm=NULL;
    }
    UsbDemod *demod=NULL;
    unsigned generation=UINT_MAX;
    uint64_t sequence=0;
    gint64 last=0, report=0;
    UsbTapBlock *copy=g_new(UsbTapBlock,1);
    gboolean was_muted=FALSE;
    while (!g_atomic_int_get(&r->stop)) {
        gboolean muted=usb_receiver_muted(r);
        if (muted!=was_muted) {
            r->speaker_used=0;
            if (r->pcm) { snd_pcm_drop(r->pcm); snd_pcm_prepare(r->pcm); }
            was_muted=muted;
        }
        g_mutex_lock(&r->mutex);
        double offset=r->offset, rate=r->rate, frequency=r->center+offset;
        gboolean active=r->source_active; unsigned current=r->generation;
        MonitorMode mode=r->mode;
        g_mutex_unlock(&r->mutex);
        usb_tci_mode(r->tci,mode); usb_tci_frequency(r->tci,frequency); usb_tci_poll(r->tci);
        if (current!=generation) {
            usb_demod_free(demod); demod=NULL; generation=current; sequence=0;
            pcm_udp_reset(r);
            atomic_store_explicit(&r->tap->read_index,atomic_load_explicit(&r->tap->write_index,memory_order_acquire),memory_order_release);
            r->speaker_used=0;
            if (r->pcm) { snd_pcm_drop(r->pcm); snd_pcm_prepare(r->pcm); }
        }
        unsigned read=atomic_load_explicit(&r->tap->read_index,memory_order_relaxed);
        unsigned write=atomic_load_explicit(&r->tap->write_index,memory_order_acquire);
        gboolean worked=FALSE;
        if (read!=write) {
            worked=TRUE;
            UsbTapBlock *b=&r->tap->blocks[read%USB_TAP_SLOTS];
            memcpy(copy,b,offsetof(UsbTapBlock,iq)+b->samples*(b->u8 ? 2 : 4));
            atomic_store_explicit(&r->tap->read_index,read+1,memory_order_release);
            if (active && copy->rate==rate && g_get_monotonic_time()-copy->time_us<150000) {
                if (sequence && copy->sequence!=sequence+1) { usb_demod_free(demod); demod=NULL; pcm_udp_reset(r); }
                sequence=copy->sequence;
                if (!demod) demod=usb_demod_new_mode(rate,offset,mode);
                if (demod) usb_demod_feed(demod,copy->iq,copy->samples,copy->u8,audio,r);
                last=g_get_monotonic_time(); worked=TRUE;
            } else { usb_demod_free(demod); demod=NULL; sequence=0; pcm_udp_reset(r); }
        }
        speaker_flush(r);
        gint64 now=g_get_monotonic_time();
        if (now-report>200000) {
            g_mutex_lock(&r->mutex);
            char udp[128]="";
            if (r->pcm_udp_fd>=0) g_snprintf(udp,sizeof(udp)," · PCM UDP %s:%u (%u send errors)",
                r->pcm_udp_host,ntohs(r->pcm_udp_address.sin_port),r->pcm_udp_errors);
            g_snprintf(r->status,sizeof(r->status),"%s · %u TCI client(s) · %u monitor blocks dropped%s%s%s%s",
                rate<monitor_min_rate(mode) ? "IQ rate too low for the selected mode" :
                active && now-last<500000 ? (mode==MONITOR_FM_DATA ? "Receiving FM-DATA · 20 kHz · flat audio" :
                    mode==MONITOR_FM ? "Receiving FM · 12.5 kHz" : "Receiving USB · 3 kHz") :
                "Waiting for IQ · start SDR / file",
                usb_tci_clients(r->tci),atomic_load_explicit(&r->tap->dropped,memory_order_relaxed),
                muted ? " · speaker muted (M)" : "",
                *r->speaker_error ? " · " : "",r->speaker_error,udp);
            g_mutex_unlock(&r->mutex); report=now;
        }
        if (!worked) g_usleep(2000);
    }
    usb_demod_free(demod); g_free(copy);
    if (r->pcm) { snd_pcm_drop(r->pcm); snd_pcm_close(r->pcm); r->pcm=NULL; }
    return NULL;
}
gboolean usb_receiver_start(UsbReceiver *r, const char *host, unsigned port, const char *device, GError **error) {
    if (!r) { g_set_error_literal(error,G_FILE_ERROR,G_FILE_ERROR_FAILED,"USB IQ buffer unavailable"); return FALSE; }
    if (r->thread) return TRUE;
    r->tci=usb_tci_new(host,port,tci_tune,r,error);
    if (!r->tci) return FALSE;
    r->device=g_strdup(device); r->speaker_used=0; r->speaker_error[0]=0;
    g_strlcpy(r->status,"Starting audio monitor / TCI…",sizeof(r->status));
    atomic_store_explicit(&r->tap->dropped,0,memory_order_relaxed);
    g_atomic_int_set(&r->stop,0);
    r->thread=g_thread_new("usb-receiver",receive,r);
    atomic_store_explicit(&r->tap->enabled,r->source_active,memory_order_relaxed);
    return TRUE;
}
void usb_receiver_stop(UsbReceiver *r) {
    if (!r || !r->thread) return;
    atomic_store_explicit(&r->tap->enabled,0,memory_order_relaxed);
    g_atomic_int_set(&r->stop,1); g_thread_join(r->thread); r->thread=NULL;
    usb_tci_free(r->tci); r->tci=NULL; g_clear_pointer(&r->device,g_free);
    pcm_udp_reset(r);
}
gboolean usb_receiver_running(UsbReceiver *r) { return r && r->thread; }
void usb_receiver_status(UsbReceiver *r, char *message, size_t size) {
    if (!r) { g_strlcpy(message,"USB unavailable",size); return; }
    g_mutex_lock(&r->mutex); g_strlcpy(message,r->thread ? r->status : "USB / FM / FM-DATA off",size); g_mutex_unlock(&r->mutex);
}
void usb_receiver_free(UsbReceiver *r) {
    if (!r) return;
    usb_receiver_stop(r); munmap(r->tap,sizeof(*r->tap)); unlink(r->path);
    if (r->pcm_udp_fd>=0) close(r->pcm_udp_fd);
    g_free(r->path); g_mutex_clear(&r->mutex); g_free(r);
}
