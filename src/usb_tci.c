#define _POSIX_C_SOURCE 200809L
#include "usb_tci.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLIENTS 8
#define INPUT_SIZE 8192
#define OUTPUT_SIZE 131072
/* Each connection has bounded input/output and a nonblocking socket. A slow
 * client is disconnected instead of delaying the receiver or other clients. */
typedef struct {
    int fd;
    gboolean ready, audio, fragmented;
    gint64 connected;
    unsigned char input[INPUT_SIZE], output[OUTPUT_SIZE];
    size_t used, queued, sent, text_used;
    char text[4096];
    unsigned rate, channels, samples, format, phase, count;
    float frame[4096];
} Client;
struct UsbTci {
    int fd;
    Client clients[CLIENTS];
    double frequency;
    MonitorMode mode;
    UsbTciTune tune;
    void *user;
};
static void disconnect(Client *c) {
    if (c->fd>=0) close(c->fd);
    c->fd=-1;
}
static void enqueue(Client *c, const void *data, size_t n) {
    if (c->fd<0) return;
    if (c->sent) {
        memmove(c->output,c->output+c->sent,c->queued-c->sent);
        c->queued-=c->sent; c->sent=0;
    }
    if (n>sizeof(c->output)-c->queued) { disconnect(c); return; }
    memcpy(c->output+c->queued,data,n); c->queued+=n;
}
static void frame(Client *c, unsigned opcode, const void *data, size_t n) {
    unsigned char h[4]={(unsigned char)(0x80|opcode),0}; size_t len=2;
    if (n<126) h[1]=n;
    else { h[1]=126; h[2]=n>>8; h[3]=n; len=4; }
    enqueue(c,h,len); enqueue(c,data,n);
}
static void text(Client *c, const char *message) { frame(c,1,message,strlen(message)); }
static void vfo(UsbTci *s, Client *c) {
    char message[96]; g_snprintf(message,sizeof(message),"vfo:0,0,%.0f;",s->frequency); text(c,message);
}
static void modulation(UsbTci *s, Client *c) {
    text(c,s->mode==MONITOR_FM_DATA ? "modulation:0,NFM;rx_filter_band:0,-10000,10000;" :
        s->mode==MONITOR_FM ? "modulation:0,NFM;rx_filter_band:0,-6250,6250;" :
        "modulation:0,USB;rx_filter_band:0,0,3000;");
}
void usb_tci_mode(UsbTci *s, MonitorMode mode) {
    if (s->mode==mode) return;
    s->mode=mode;
    for (unsigned i=0;i<CLIENTS;++i) {
        Client *c=&s->clients[i]; c->count=c->phase=0;
        if (c->fd>=0 && c->ready) {
            /* DATA is not speech-bandlimited: never downsample it by dropping
             * samples. Announce 48 kHz before any audio in the new mode. */
            if (mode==MONITOR_FM_DATA && c->rate!=48000) {
                c->rate=48000; text(c,"audio_samplerate:48000;");
            }
            modulation(s,c);
        }
    }
}
static void initial(UsbTci *s, Client *c) {
    text(c,"protocol:ExpertSDR3,1.9.0;device:Linrad-DVB-Monitor;trx_count:1;channel_count:1;"
        "receive_only:true;trx:0,false;rx_enable:0,true;"
        "audio_samplerate:48000;audio_stream_sample_type:float32;"
        "audio_stream_channels:1;audio_stream_samples:2048;");
    modulation(s,c); vfo(s,c); text(c,"ready;start;");
}
static gboolean number(const char *text, unsigned *value) {
    char *end; errno=0; unsigned long v=strtoul(text,&end,10);
    if (errno || end==text || *end || v>UINT32_MAX) return FALSE;
    *value=(unsigned)v; return TRUE;
}
static void command(UsbTci *s, Client *c, char *cmd) {
    cmd=g_strstrip(cmd);
    for (char *p=cmd;*p;++p) *p=g_ascii_tolower(*p);
    char *arg=strchr(cmd,':');
    if (arg) *arg++=0;
    else arg="";
    if (!strcmp(cmd,"vfo")) {
        if (!strncmp(arg,"0,0,",4)) {
            char *end; double hz=g_ascii_strtod(arg+4,&end);
            if (end!=arg+4 && !*end && isfinite(hz) && hz>=0 && s->tune)
                usb_tci_frequency(s,s->tune(hz,s->user));
        }
        vfo(s,c);
    } else if (!strcmp(cmd,"audio_start") && !strcmp(arg,"0")) {
        c->audio=TRUE; c->count=c->phase=0; text(c,"audio_start:0;");
    } else if (!strcmp(cmd,"audio_stop") && !strcmp(arg,"0")) {
        c->audio=FALSE; c->count=0; text(c,"audio_stop:0;");
    } else if (!strcmp(cmd,"audio_samplerate")) {
        unsigned v; if (number(arg,&v) && (v==48000 || v==24000 || v==12000))
            c->rate=s->mode==MONITOR_FM_DATA ? 48000 : v;
        c->count=c->phase=0;
        char reply[64]; g_snprintf(reply,sizeof(reply),"audio_samplerate:%u;",c->rate); text(c,reply);
    } else if (!strcmp(cmd,"audio_stream_channels")) {
        unsigned v; if (number(arg,&v) && (v==1 || v==2)) c->channels=v;
        c->count=0;
        char reply[64]; g_snprintf(reply,sizeof(reply),"audio_stream_channels:%u;",c->channels); text(c,reply);
    } else if (!strcmp(cmd,"audio_stream_samples")) {
        unsigned v; if (number(arg,&v) && v>=64 && v<=4096) c->samples=v;
        c->count=0;
        char reply[64]; g_snprintf(reply,sizeof(reply),"audio_stream_samples:%u;",c->samples); text(c,reply);
    } else if (!strcmp(cmd,"audio_stream_sample_type")) {
        if (!strcmp(arg,"int16")) c->format=0;
        else c->format=3;
        c->count=0; text(c,c->format ? "audio_stream_sample_type:float32;" : "audio_stream_sample_type:int16;");
    } else if (!strcmp(cmd,"trx")) text(c,"trx:0,false;receive_only:true;");
    else if (!strcmp(cmd,"modulation") || !strcmp(cmd,"rx_filter_band")) modulation(s,c);
    else if (!strcmp(cmd,"rx_enable")) text(c,"rx_enable:0,true;");
    else if (!strcmp(cmd,"start")) text(c,"start;");
    else if (!strcmp(cmd,"stop")) { c->audio=FALSE; text(c,"stop;"); }
}
static void commands(UsbTci *s, Client *c) {
    char *begin=c->text, *end;
    while ((end=strchr(begin,';'))) { *end=0; command(s,c,begin); begin=end+1; }
    size_t remain=strlen(begin); memmove(c->text,begin,remain+1); c->text_used=remain;
}
static void handshake(UsbTci *s, Client *c) {
    c->input[c->used]=0;
    char *end=strstr((char *)c->input,"\r\n\r\n");
    if (!end) return;
    size_t consumed=(size_t)(end-(char *)c->input)+4;
    char *request=g_strndup((char *)c->input,consumed);
    char **lines=g_strsplit(request,"\r\n",-1);
    const char *key=NULL; gboolean upgrade=FALSE, connection=FALSE, version=FALSE;
    for (unsigned i=1;lines[i];++i) {
        char *colon=strchr(lines[i],':'); if (!colon) continue;
        *colon++=0; colon=g_strstrip(colon);
        if (!g_ascii_strcasecmp(lines[i],"Sec-WebSocket-Key")) key=colon;
        if (!g_ascii_strcasecmp(lines[i],"Sec-WebSocket-Version")) version=!strcmp(colon,"13");
        if (!g_ascii_strcasecmp(lines[i],"Upgrade")) upgrade=!g_ascii_strcasecmp(colon,"websocket");
        if (!g_ascii_strcasecmp(lines[i],"Connection")) {
            char *lower=g_ascii_strdown(colon,-1); connection=strstr(lower,"upgrade")!=NULL; g_free(lower);
        }
    }
    gsize key_size=0; guchar *decoded=key ? g_base64_decode(key,&key_size) : NULL;
    if (strncmp(request,"GET ",4) || !key || key_size!=16 || !upgrade || !connection || !version) disconnect(c);
    else {
        char *joined=g_strconcat(key,"258EAFA5-E914-47DA-95CA-C5AB0DC85B11",NULL);
        GChecksum *sha=g_checksum_new(G_CHECKSUM_SHA1); g_checksum_update(sha,(const guchar *)joined,strlen(joined));
        guchar digest[20]; gsize n=sizeof(digest); g_checksum_get_digest(sha,digest,&n);
        char *accept=g_base64_encode(digest,n);
        char *response=g_strdup_printf("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",accept);
        enqueue(c,response,strlen(response)); c->ready=TRUE; initial(s,c);
        g_free(response); g_free(accept); g_free(joined); g_checksum_free(sha);
    }
    g_free(decoded); g_strfreev(lines); g_free(request);
    memmove(c->input,c->input+consumed,c->used-consumed); c->used-=consumed;
}
static void parse(UsbTci *s, Client *c) {
    while (c->fd>=0 && c->used>=2) {
        unsigned char *p=c->input; unsigned op=p[0]&15; gboolean fin=p[0]&128;
        size_t n=p[1]&127, h=2;
        if ((p[0]&0x70) || !(p[1]&128)) { disconnect(c); break; }
        if (n==126) { if (c->used<4) break; n=(unsigned)p[2]*256+p[3]; h=4; }
        else if (n==127) { disconnect(c); break; }
        if (n>4096 || (op>=8 && (!fin || n>125))) { disconnect(c); break; }
        if (c->used<h+4+n) break;
        unsigned char *data=p+h+4;
        for (size_t i=0;i<n;++i) data[i]^=p[h+i%4];
        if (op==8) { disconnect(c); break; }
        else if (op==9) frame(c,10,data,n);
        else if (op==10) { /* Pong. */ }
        else if (op==1 || op==0) {
            if ((op==0 && !c->fragmented) || (op==1 && c->fragmented) ||
                n>=sizeof(c->text)-c->text_used || memchr(data,0,n)) { disconnect(c); break; }
            memcpy(c->text+c->text_used,data,n); c->text_used+=n; c->text[c->text_used]=0;
            c->fragmented=!fin;
            if (fin) {
                if (!g_utf8_validate(c->text,c->text_used,NULL)) { disconnect(c); break; }
                commands(s,c);
            }
        } else if (op!=2) { disconnect(c); break; }
        size_t consumed=h+4+n;
        memmove(c->input,c->input+consumed,c->used-consumed); c->used-=consumed;
    }
}
UsbTci *usb_tci_new(const char *host, unsigned port, UsbTciTune tune, void *user, GError **error) {
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(port)};
    if (port>65535 || !port || inet_pton(AF_INET,host,&address.sin_addr)!=1) {
        g_set_error_literal(error,G_FILE_ERROR,G_FILE_ERROR_INVAL,"USB TCI: invalid IPv4 bind address or port"); return NULL;
    }
    int fd=socket(AF_INET,SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,0), yes=1;
    if (fd>=0) setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    if (fd<0 || bind(fd,(struct sockaddr *)&address,sizeof(address))<0 || listen(fd,8)<0) {
        int saved=errno; if (fd>=0) close(fd);
        g_set_error(error,G_FILE_ERROR,g_file_error_from_errno(saved),"USB TCI %s:%u: %s",host,port,g_strerror(saved)); return NULL;
    }
    UsbTci *s=g_new0(UsbTci,1); s->fd=fd; s->tune=tune; s->user=user;
    for (unsigned i=0;i<CLIENTS;++i) s->clients[i].fd=-1;
    return s;
}
void usb_tci_free(UsbTci *s) {
    if (!s) return;
    for (unsigned i=0;i<CLIENTS;++i) disconnect(&s->clients[i]);
    close(s->fd); g_free(s);
}
void usb_tci_poll(UsbTci *s) {
    for (unsigned i=0;i<CLIENTS;++i) {
        Client *c=&s->clients[i];
        if (c->fd<0) {
            int fd=accept(s->fd,NULL,NULL);
            if (fd<0) continue;
            /* accept() does not inherit O_NONBLOCK on Linux. */
            int flags=fcntl(fd,F_GETFL,0);
            if (flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0 || fcntl(fd,F_SETFD,FD_CLOEXEC)<0) { close(fd); continue; }
            int buffer=32768, yes=1;
            setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&buffer,sizeof(buffer));
            setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));
            memset(c,0,sizeof(*c)); c->fd=fd; c->connected=g_get_monotonic_time();
            c->rate=48000; c->channels=1; c->samples=2048; c->format=3;
        }
        if (!c->ready && g_get_monotonic_time()-c->connected>5000000) { disconnect(c); continue; }
        ssize_t n=recv(c->fd,c->input+c->used,sizeof(c->input)-1-c->used,MSG_DONTWAIT);
        if (!n || (n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)) { disconnect(c); continue; }
        if (n>0) {
            c->used+=n;
            if (!c->ready) handshake(s,c);
            if (c->ready) parse(s,c);
            if (c->used==sizeof(c->input)-1) disconnect(c);
        }
        if (c->fd>=0 && c->queued>c->sent) {
            n=send(c->fd,c->output+c->sent,c->queued-c->sent,MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n>0) { c->sent+=n; if (c->sent==c->queued) c->sent=c->queued=0; }
            else if (n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) disconnect(c);
        }
    }
}
void usb_tci_frequency(UsbTci *s, double frequency) {
    if (s->frequency==frequency) return;
    s->frequency=frequency;
    for (unsigned i=0;i<CLIENTS;++i) if (s->clients[i].fd>=0 && s->clients[i].ready) vfo(s,&s->clients[i]);
}
static void put32(unsigned char *p, uint32_t value) {
    value=GUINT32_TO_LE(value); memcpy(p,&value,4);
}
void usb_tci_audio(UsbTci *s, const float *audio, size_t count, float gain) {
    for (unsigned i=0;i<CLIENTS;++i) {
        Client *c=&s->clients[i];
        if (c->fd<0 || !c->ready || !c->audio) continue;
        for (size_t j=0;j<count;++j) {
            if (++c->phase<48000/c->rate) continue;
            c->phase=0; c->frame[c->count++]=audio[j]*gain;
            if (c->count<c->samples) continue;
            unsigned char packet[64+4096*2*4]={0};
            put32(packet+4,c->rate); put32(packet+8,c->format); put32(packet+20,c->samples);
            put32(packet+24,1); put32(packet+28,c->channels);
            unsigned char *p=packet+64;
            for (unsigned k=0;k<c->samples;++k) for (unsigned ch=0;ch<c->channels;++ch) {
                if (c->format==3) { uint32_t bits; memcpy(&bits,&c->frame[k],4); put32(p,bits); p+=4; }
                else { int16_t v=(int16_t)lrintf(c->frame[k]*32767); *p++=v&255; *p++=(uint16_t)v>>8; }
            }
            frame(c,2,packet,(size_t)(p-packet)); c->count=0;
            if (c->fd<0) break;
        }
    }
}
unsigned usb_tci_clients(UsbTci *s) {
    unsigned n=0; for (unsigned i=0;i<CLIENTS;++i) if (s->clients[i].fd>=0 && s->clients[i].ready) ++n;
    return n;
}
