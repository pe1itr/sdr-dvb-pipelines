#define _POSIX_C_SOURCE 200809L
#include "dsmcc_output.h"
#include "psi.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

/* PAT and PMT section_length is at most 1021. PMTs may share a PID. */
typedef struct {
    guint8 bytes[1024];
    unsigned used;
    int cc;
} PsiAssembly;
typedef struct {
    guint16 pmt;
    GArray *data_pids;
    guint32 crc;
    gboolean received;
} Program;

struct DsmccOutput {
    int fd, pat_version;
    guint16 tsid;
    gboolean pat_seen[256];
    PsiAssembly pat;
    GHashTable *programs, *pmts;
    guint32 owners[8192];
    guint8 packet[376], datagram[7*188];
    unsigned used, queued;
    gboolean synced, warned;
};

static guint16 word(const guint8 *p) { return ((guint16)p[0]<<8) | p[1]; }
static void program_free(gpointer ptr) {
    Program *program=ptr;
    g_array_unref(program->data_pids);
    g_free(program);
}
static void clear_map(DsmccOutput *out) {
    g_hash_table_remove_all(out->programs);
    g_hash_table_remove_all(out->pmts);
    memset(out->owners,0,sizeof(out->owners));
    memset(out->pat_seen,0,sizeof(out->pat_seen));
    out->pat_version=-1;
}
void dsmcc_output_reset(DsmccOutput *out) {
    if (!out) return;
    clear_map(out);
    out->pat=(PsiAssembly){.cc=-1};
    out->used=out->queued=0;
    out->synced=FALSE;
}
DsmccOutput *dsmcc_output_open(const char *host, int port, GError **error) {
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(port)};
    if (!host || inet_pton(AF_INET,host,&addr.sin_addr)!=1 ||
        addr.sin_addr.s_addr==INADDR_ANY || addr.sin_addr.s_addr==INADDR_BROADCAST || port<1 || port>65535) {
        g_set_error_literal(error,G_FILE_ERROR,G_FILE_ERROR_INVAL,"Invalid DSM-CC UDP destination");
        return NULL;
    }
    int fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if (fd<0 || connect(fd,(struct sockaddr *)&addr,sizeof(addr))<0) {
        g_set_error(error,G_FILE_ERROR,g_file_error_from_errno(errno),"DSM-CC UDP: %s",g_strerror(errno));
        if (fd>=0) close(fd);
        return NULL;
    }
    DsmccOutput *out=g_new0(DsmccOutput,1);
    out->fd=fd;
    out->programs=g_hash_table_new_full(g_direct_hash,g_direct_equal,NULL,program_free);
    out->pmts=g_hash_table_new_full(g_direct_hash,g_direct_equal,NULL,g_free);
    dsmcc_output_reset(out);
    return out;
}
void dsmcc_output_free(DsmccOutput *out) {
    if (!out) return;
    close(out->fd);
    g_hash_table_unref(out->pmts);
    g_hash_table_unref(out->programs);
    g_free(out);
}

static void read_pat(DsmccOutput *out,const guint8 *b,unsigned n) {
    if ((n-12)%4 || b[6]>b[7]) return;
    unsigned version=(b[5]>>1)&31;
    if (out->pat_version!=(int)version || out->tsid!=word(b+3)) {
        clear_map(out);
        out->tsid=word(b+3);
        out->pat_version=version;
    }
    if (out->pat_seen[b[6]]) return;
    out->pat_seen[b[6]]=TRUE;
    for (unsigned at=8;at<n-4;at+=4) {
        unsigned service=word(b+at), pid=word(b+at+2)&0x1fff;
        if (!service || pid<0x10 || pid==0x1fff ||
            g_hash_table_contains(out->programs,GUINT_TO_POINTER(service))) continue;
        Program *program=g_new0(Program,1);
        program->pmt=pid;
        program->data_pids=g_array_new(FALSE,FALSE,sizeof(guint16));
        g_hash_table_insert(out->programs,GUINT_TO_POINTER(service),program);
        if (!g_hash_table_contains(out->pmts,GUINT_TO_POINTER(pid))) {
            PsiAssembly *assembly=g_new0(PsiAssembly,1);
            assembly->cc=-1;
            g_hash_table_insert(out->pmts,GUINT_TO_POINTER(pid),assembly);
        }
    }
}
static gboolean descriptors(const guint8 *data,unsigned n,gboolean *carousel) {
    while (n>=2 && data[1]<=n-2) {
        unsigned size=2+data[1];
        if (data[0]==0x66 && size>=4 && (word(data+2)==6 || word(data+2)==7))
            *carousel=TRUE;
        data+=size;
        n-=size;
    }
    return n==0;
}
static void read_pmt(DsmccOutput *out,unsigned pid,const guint8 *b,unsigned n) {
    Program *program=g_hash_table_lookup(out->programs,GUINT_TO_POINTER((unsigned)word(b+3)));
    if (!program || program->pmt!=pid || n<16 || b[6] || b[7]) return;
    guint32 crc=((guint32)word(b+n-4)<<16)|word(b+n-2);
    if (program->received && program->crc==crc) return;
    unsigned at=12+(word(b+10)&0xfff);
    gboolean unused=FALSE;
    if (at>n-4 || !descriptors(b+12,at-12,&unused)) return;
    guint16 found[202]; /* at most (1024-16)/5 ES entries */
    unsigned count=0;
    while (at<n-4) {
        if (n-4-at<5) return;
        unsigned type=b[at], es=word(b+at+1)&0x1fff, len=word(b+at+3)&0xfff;
        gboolean carousel=FALSE;
        if (len>n-4-at-5 || !descriptors(b+at+5,len,&carousel)) return;
        if (es>=0x10 && es!=0x1fff && (type==0x08 || (type>=0x0a && type<=0x0d) || carousel))
            found[count++]=es;
        at+=5+len;
    }
    for (unsigned i=0;i<program->data_pids->len;i++)
        out->owners[g_array_index(program->data_pids,guint16,i)]--;
    g_array_set_size(program->data_pids,0);
    g_array_append_vals(program->data_pids,found,count);
    for (unsigned i=0;i<count;i++) out->owners[found[i]]++;
    program->received=TRUE;
    program->crc=crc;
}
static void sections(DsmccOutput *out,unsigned pid,PsiAssembly *a,const guint8 *p,unsigned n,gboolean starts) {
    while (n) {
        if (!a->used && (!starts || *p==0xff)) return;
        unsigned need=3;
        if (a->used>=3) need=3+(word(a->bytes+1)&0xfff);
        if (need>sizeof(a->bytes) || need<=a->used) { a->used=0; return; }
        unsigned take=MIN(n,need-a->used);
        memcpy(a->bytes+a->used,p,take);
        a->used+=take; p+=take; n-=take;
        if (a->used<3) continue;
        unsigned size=3+(word(a->bytes+1)&0xfff);
        if (size<12 || size>sizeof(a->bytes)) { a->used=0; return; }
        if (size==a->used) {
            const guint8 *b=a->bytes;
            if ((b[1]&0x80) && (b[5]&1) && !psi_mpeg_crc32(b,size)) {
                if (!pid && !b[0]) read_pat(out,b,size);
                else if (pid && b[0]==2) read_pmt(out,pid,b,size);
            }
            a->used=0;
            if (!starts) return;
        }
    }
}
static void flush(DsmccOutput *out) {
    if (!out->queued) return;
    if (send(out->fd,out->datagram,out->queued,MSG_DONTWAIT)<0 && !out->warned) {
        out->warned=TRUE;
        g_printerr("[DSM-CC] UDP send failed: %s\n",g_strerror(errno));
    }
    out->queued=0;
}
static void packet(DsmccOutput *out,const guint8 *bytes) {
    unsigned pid=word(bytes+1)&0x1fff;
    if (out->owners[pid]) {
        memcpy(out->datagram+out->queued,bytes,188);
        out->queued+=188;
        if (out->queued==sizeof(out->datagram)) flush(out);
        return;
    }
    PsiAssembly *a=pid ? g_hash_table_lookup(out->pmts,GUINT_TO_POINTER(pid)) : &out->pat;
    if (!a) return;
    TsPacket ts;
    if (!ts_packet_parse(bytes,&ts) || ts.transport_error || ts.scrambling) {
        a->used=0; a->cc=-1; return;
    }
    if (ts.discontinuity_indicator) { a->used=0; a->cc=-1; }
    if (!ts.payload_size || a->cc==ts.continuity_counter) return;
    if (((a->cc+1)&15)!=ts.continuity_counter) a->used=0;
    a->cc=ts.continuity_counter;
    const guint8 *p=bytes+ts.payload_offset;
    unsigned n=ts.payload_size;
    if (ts.payload_unit_start) {
        unsigned prefix=*p++; n--;
        if (prefix>n) { a->used=0; return; }
        if (a->used) sections(out,pid,a,p,prefix,FALSE);
        a->used=0;
        sections(out,pid,a,p+prefix,n-prefix,TRUE);
    } else if (a->used) sections(out,pid,a,p,n,FALSE);
}
void dsmcc_output_feed(DsmccOutput *out,const guint8 *bytes,gsize size) {
    while (size) {
        if (out->synced && !out->used && size>=188 && bytes[0]==0x47) {
            packet(out,bytes); bytes+=188; size-=188; continue;
        }
        unsigned take=MIN(size,sizeof(out->packet)-out->used);
        memcpy(out->packet+out->used,bytes,take);
        out->used+=take; bytes+=take; size-=take;
        unsigned at=0;
        while (out->used-at>=188) {
            if (out->packet[at]!=0x47) {
                if (out->synced) { clear_map(out); out->pat=(PsiAssembly){.cc=-1}; }
                out->synced=FALSE; at++; continue;
            }
            if (!out->synced) {
                if (out->used-at==188) break;
                if (out->packet[at+188]!=0x47) { at++; continue; }
                out->synced=TRUE;
            }
            packet(out,out->packet+at); at+=188;
        }
        out->used-=at;
        memmove(out->packet,out->packet+at,out->used);
    }
    flush(out);
}
