#define _POSIX_C_SOURCE 200809L
#include "iq_file.h"
#include <glib/gstdio.h>
#include "iq_bandfilter.h"
#include "iq_monitor.h"
#include "iq_shift.h"
#include "tune_control.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop_signal(int sig) { (void)sig; stopping=1; }

typedef struct {
    gboolean control;
    int rate;
    double bandwidth;
    IQShift shift;
    TuneControl parser;
    IQBandFilter *filter;
} Replay;
static gboolean controls(Replay *r) {
    if (!r->control) return TRUE;
    double unused=0, bandwidth=r->bandwidth;
    int changed=tune_control_read(&r->parser,STDIN_FILENO,1,0,&unused,1,0,0,&unused,r->rate,&bandwidth);
    if ((changed&4) && bandwidth!=r->bandwidth) {
        if (r->shift.offset && !iq_shift_fits(r->rate,r->shift.offset,bandwidth)) {
            fprintf(stderr,"[iq-file] Filter exceeds recorded band at selected offset\n");
            return TRUE;
        }
        if (iq_bandfilter_set(r->filter,bandwidth)) return FALSE;
        r->bandwidth=bandwidth;
        fprintf(stderr,"[iq-file] IQ filter width %.0f Hz (0=bypass)\n",bandwidth);
    }
    return TRUE;
}
static gboolean wait_until(Replay *r, gint64 deadline) {
    while (!stopping) {
        if (!controls(r)) return FALSE;
        gint64 remaining=deadline-g_get_monotonic_time();
        if (remaining<=0) break;
        struct pollfd fd={STDIN_FILENO,POLLIN,0};
        /* Short bounded waits keep Stop and filter commands responsive. */
        int result=poll(r->control ? &fd : NULL,r->control ? 1 : 0,(int)MIN(20,(remaining+999)/1000));
        if (result<0 && errno!=EINTR) return FALSE;
        if (r->control && result>0 && (fd.revents&POLLHUP)) {
            /* A closed writer can still have a final command queued. */
            if (!controls(r)) return FALSE;
            r->control=FALSE;
        }
    }
    return TRUE;
}
static gboolean output(Replay *r, const uint8_t *buf, size_t length) {
    while (length && !stopping) {
        if (!controls(r)) return FALSE;
        ssize_t n=write(STDOUT_FILENO,buf,length);
        if (n>0) { buf+=n; length-=n; continue; }
        if (n<0 && errno==EINTR) continue;
        if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) {
            struct pollfd fd={STDOUT_FILENO,POLLOUT,0};
            if (poll(&fd,1,20)<0 && errno!=EINTR) return FALSE;
            continue;
        }
        return FALSE;
    }
    return TRUE;
}
int main(int argc, char **argv) {
#ifdef _WIN32
    argv=g_win32_get_command_line();
#endif
    char *path=NULL, *format=NULL;
    int header=0; Replay r={0};
    double offset=0;
    GOptionEntry entries[]={
        {"in",0,0,G_OPTION_ARG_FILENAME,&path,"IQ file","FILE"},
        {"sample-rate",0,0,G_OPTION_ARG_INT,&r.rate,"Samples per second","HZ"},
        {"input-format",0,0,G_OPTION_ARG_STRING,&format,"s16, u8 or cf32 (little-endian IQ)","FORMAT"},
        {"header-bytes",0,0,G_OPTION_ARG_INT,&header,"Bytes to skip at the start","N"},
        {"receiver-offset",0,0,G_OPTION_ARG_DOUBLE,&offset,"Channel offset from recording center","HZ"},
        {"filter-bandwidth",0,0,G_OPTION_ARG_DOUBLE,&r.bandwidth,"Total IQ bandwidth; 0=bypass","HZ"},
        {"control-stdin",0,0,G_OPTION_ARG_NONE,&r.control,"Live bandwidth commands on stdin",NULL}, {NULL}
    };
    GOptionContext *options=g_option_context_new("- Play an IQ file to stdout in real time");
    g_option_context_add_main_entries(options,entries,NULL);
    GError *error=NULL; guint64 samples=0;
#ifdef _WIN32
    if (!g_option_context_parse_strv(options,&argv,&error)) goto invalid;
    argc=(int)g_strv_length(argv);
#else
    if (!g_option_context_parse(options,&argc,&argv,&error)) goto invalid;
#endif
    if (!format) format=g_strdup("s16");
    if (argc!=1 || !path || r.rate<1 || r.rate>100000000 || !iq_bandfilter_valid(r.rate,r.bandwidth)) {
        g_set_error_literal(&error,G_OPTION_ERROR,G_OPTION_ERROR_BAD_VALUE,"Invalid file, sample rate or bandwidth"); goto invalid;
    }
    if (iq_shift_init(&r.shift,r.rate,offset) || (offset && !iq_shift_fits(r.rate,offset,r.bandwidth))) {
        g_set_error_literal(&error,G_OPTION_ERROR,G_OPTION_ERROR_BAD_VALUE,"Receiver offset or filter exceeds recorded band"); goto invalid;
    }
    if (!iq_file_validate(path,format,header,&samples,&error)) goto invalid;
    FILE *file=g_fopen(path,"rb");
    if (!file || fseeko(file,header,SEEK_SET)) { perror("Open IQ file"); if (file) fclose(file); return 1; }
    struct sigaction sa={.sa_handler=stop_signal}; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL); signal(SIGPIPE,SIG_IGN);
    int flags=fcntl(STDOUT_FILENO,F_GETFL);
    if (flags<0 || fcntl(STDOUT_FILENO,F_SETFL,flags|O_NONBLOCK)<0) { perror("stdout"); fclose(file); return 1; }
    if (r.control) {
        flags=fcntl(STDIN_FILENO,F_GETFL);
        if (flags<0 || fcntl(STDIN_FILENO,F_SETFL,flags|O_NONBLOCK)<0) { perror("stdin"); fclose(file); return 1; }
    }
    r.filter=iq_bandfilter_create(r.rate,r.bandwidth);
    if (!r.filter) { fprintf(stderr,"[iq-file] Cannot create IQ filter\n"); fclose(file); return 1; }
    gboolean u8=!strcmp(format,"u8"), cf32=!strcmp(format,"cf32");
    size_t stride=u8 ? 2 : cf32 ? 8 : 4, chunk=CLAMP(r.rate/100,1,8192);
    uint8_t input[8192*8], converted[8192*4];
    float shifted[8192*2];
    guint64 sent=0; int result=0;
    gint64 start=g_get_monotonic_time(), epoch=start;
    fprintf(stderr,"[iq-file] %s: %s, %d samples/s, %.3f s, realtime\n",path,format,r.rate,(double)samples/r.rate);
    while (!stopping && sent<samples) {
        size_t count=(size_t)MIN((guint64)chunk,samples-sent);
        if (fread(input,stride,count,file)!=count) { fprintf(stderr,"[iq-file] Unexpected end of file or read error\n"); result=1; break; }
        uint8_t *out=input;
        if (cf32) {
            for (size_t i=0; i<count*2; ++i) {
                uint32_t bits; float value; int16_t sample;
                memcpy(&bits,input+i*4,4); bits=GUINT32_FROM_LE(bits); memcpy(&value,&bits,4);
                if (!isfinite(value)) { fprintf(stderr,"[iq-file] Non-finite float in IQ file\n"); result=1; break; }
                value=fmaxf(-1,fminf(1,value)); sample=(int16_t)lrintf(value*32767.0f);
                converted[i*2]=(uint16_t)sample&255; converted[i*2+1]=(uint16_t)sample>>8;
            }
            if (result) break;
            out=converted;
        }
        if (!controls(&r)) { result=1; break; }
        iq_monitor_feed(out,count,u8,r.rate);
        if (offset) {
            for (size_t i=0; i<count*2; ++i)
                shifted[i]=u8 ? ((int)out[i]-128)*256 : (int16_t)(out[2*i] | (out[2*i+1]<<8));
            iq_shift_process(&r.shift,shifted,count);
            iq_bandfilter_process_float(r.filter,shifted,count);
            iq_shift_pack(shifted,out,count,u8);
        } else iq_bandfilter_process(r.filter,out,count,u8);
        if (!output(&r,out,count*(u8 ? 2 : 4))) { if (!stopping) { perror("IQ stdout"); result=1; } break; }
        sent+=count;
        gint64 deadline=epoch+(gint64)((long double)sent*G_USEC_PER_SEC/r.rate);
        /* If downstream stalls, resume at real time instead of dumping a burst. */
        gint64 now=g_get_monotonic_time();
        if (now>deadline+100000) { epoch+=now-deadline; deadline=now; }
        if (!wait_until(&r,deadline)) { result=1; break; }
    }
    fprintf(stderr,"[iq-file] %s: %" G_GUINT64_FORMAT " samples, %.3f s elapsed\n",
        stopping ? "stopped" : result ? "error" : "end of file",sent,(g_get_monotonic_time()-start)/1e6);
    iq_bandfilter_destroy(r.filter); fclose(file);
    g_free(path); g_free(format); g_option_context_free(options); return result;
invalid:
    fprintf(stderr,"[iq-file] %s\n",error ? error->message : "Invalid arguments");
    g_clear_error(&error); g_free(path); g_free(format); g_option_context_free(options); return 2;
}
