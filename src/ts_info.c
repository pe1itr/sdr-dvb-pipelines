#include "ts_info.h"
#include <libzvbi.h>
#include <string.h>

/* Only five PIDs are assembled, and all allocations/queues have fixed bounds. */
typedef struct {
    guint8 data[65542], previous[188];
    gsize used, expected;
    int cc;
} Assembly;
struct TsInfoParser {
    TsInfoSnapshot state;
    Assembly pat, sdt, eit, pmt, pes;
    guint8 packet[376];
    unsigned packet_used;
    gboolean synchronized;
    guint16 tsid, ttx_pid;
    int pat_version, eit_version;
    TsInfoService pending[TS_INFO_SERVICES];
    unsigned pending_count;
    guint8 pat_sections[256];
    vbi_decoder *teletext;
    double teletext_time;
    /* libzvbi owns the decoded pages. Remember which subpage arrived last. */
    struct { int subno; gint64 received; } latest[800];
};

static guint16 u16(const guint8 *p) { return ((guint16)p[0]<<8)|p[1]; }
static guint32 crc32(const guint8 *data, gsize size) {
    guint32 crc=0xffffffff;
    for (gsize i=0;i<size;i++) {
        crc^=(guint32)data[i]<<24;
        for (int j=0;j<8;j++) crc=crc&0x80000000 ? (crc<<1)^0x04c11db7 : crc<<1;
    }
    return crc;
}
static void page_clear(TsInfoSnapshot *s) {
    g_clear_pointer(&s->page,g_bytes_unref);
    g_clear_pointer(&s->page_flash_off,g_bytes_unref);
    g_clear_pointer(&s->page_reveal,g_bytes_unref);
    g_clear_pointer(&s->page_reveal_flash_off,g_bytes_unref);
    memset(s->color_pages,0,sizeof(s->color_pages));
    memset(s->footer_links,0,sizeof(s->footer_links));
    s->width=s->height=0;
}
void ts_info_snapshot_clear(TsInfoSnapshot *s) {
    g_clear_pointer(&s->pmt_section,g_bytes_unref);
    page_clear(s);
    memset(s,0,sizeof(*s));
}
static void snapshot_copy(TsInfoSnapshot *out, const TsInfoSnapshot *in) {
    *out=*in;
    if (out->pmt_section) g_bytes_ref(out->pmt_section);
    if (out->page) g_bytes_ref(out->page);
    if (out->page_flash_off) g_bytes_ref(out->page_flash_off);
    if (out->page_reveal) g_bytes_ref(out->page_reveal);
    if (out->page_reveal_flash_off) g_bytes_ref(out->page_reveal_flash_off);
}
void ts_info_parser_snapshot(TsInfoParser *p, TsInfoSnapshot *out) { snapshot_copy(out,&p->state); }
static void assembly_clear(Assembly *a) { a->used=a->expected=0; a->cc=-1; }
static void teletext_clear(TsInfoParser *p) {
    if (p->teletext) vbi_decoder_delete(p->teletext);
    memset(p->latest,0,sizeof(p->latest));
    p->teletext=NULL; p->teletext_time=0; assembly_clear(&p->pes);
}
static void content_clear(TsInfoParser *p) {
    memset(p->state.events,0,sizeof(p->state.events));
    p->state.eit_received=p->state.teletext_received=0;
    page_clear(&p->state);
    g_clear_pointer(&p->state.pmt_section,g_bytes_unref);
    p->state.pmt_seen=p->state.teletext_announced=FALSE;
    p->state.video_codec[0]=p->state.audio_codec[0]=0;
    p->ttx_pid=0x1fff; p->eit_version=-1;
    assembly_clear(&p->eit); assembly_clear(&p->pmt); teletext_clear(p);
}
TsInfoParser *ts_info_parser_new(void) {
    TsInfoParser *p=g_new0(TsInfoParser,1);
    p->pat_version=-1;
    p->state.teletext_decoder=TRUE;
    p->state.page_number=100;
    assembly_clear(&p->pat); assembly_clear(&p->sdt); content_clear(p);
    return p;
}
void ts_info_parser_free(TsInfoParser *p) {
    if (!p) return;
    teletext_clear(p); ts_info_snapshot_clear(&p->state); g_free(p);
}
static void assemblies_gap(TsInfoParser *p) {
    assembly_clear(&p->pat); assembly_clear(&p->sdt); assembly_clear(&p->eit);
    assembly_clear(&p->pmt); teletext_clear(p);
    /* Keep displayed data, but never combine sections across a loss. */
    p->pat_version=-1;
}
void ts_info_parser_gap(TsInfoParser *p) {
    assemblies_gap(p); p->packet_used=0; p->synchronized=FALSE;
}
void ts_info_parser_select(TsInfoParser *p, guint16 service) {
    for (unsigned i=0;i<p->state.count;i++) if (p->state.services[i].id==service) {
        if (service!=p->state.selected) { content_clear(p); p->state.selected=service; }
        return;
    }
}
static void show_cached_page(TsInfoParser *p);
void ts_info_parser_set_page(TsInfoParser *p, unsigned page) {
    if (page<100 || page>899 || page==p->state.page_number) return;
    p->state.page_number=page;
    page_clear(&p->state); p->state.teletext_received=0;
    show_cached_page(p);
}

/* DVB Annex A: selectors plus ISO-6937 default; never pass control codes or
 * invalid UTF-8 to GTK. Unsupported encodings use replacement characters. */
static char *dvb_text(const guint8 *s, gsize n) {
    const char *encoding="ISO_6937";
    char iso[24];
    if (n && s[0]<0x20) {
        unsigned c=*s++; n--;
        if (c>=1 && c<=11 && c!=8) {
            g_snprintf(iso,sizeof(iso),"ISO-8859-%u",c+4); encoding=iso;
        } else if (c==0x10 && n>=2 && !s[0] && s[1]>=1 && s[1]<=15) {
            g_snprintf(iso,sizeof(iso),"ISO-8859-%u",s[1]); encoding=iso; s+=2; n-=2;
        } else if (c==0x11) encoding="UTF-16BE";
        else if (c==0x12) encoding="EUC-KR";
        else if (c==0x13) encoding="GB2312";
        else if (c==0x14) encoding="BIG5";
        else if (c==0x15) encoding="UTF-8";
        else return g_strdup("[Unsupported DVB text encoding]");
    }
    GString *clean=g_string_sized_new(n);
    gboolean single=strcmp(encoding,"UTF-8") && strcmp(encoding,"UTF-16BE") &&
        strcmp(encoding,"EUC-KR") && strcmp(encoding,"GB2312") && strcmp(encoding,"BIG5");
    for (gsize i=0;i<n;i++) {
        if (single && s[i]>=0x80 && s[i]<=0x9f) {
            if (s[i]==0x8a) g_string_append_c(clean,'\n');
        } else g_string_append_c(clean,s[i]);
    }
    gsize written=0;
    char *converted=g_convert_with_fallback(clean->str,clean->len,"UTF-8",encoding,"�",NULL,&written,NULL);
    if (!converted) { converted=g_utf8_make_valid(clean->str,clean->len); written=strlen(converted); }
    g_string_free(clean,TRUE);
    GString *out=g_string_new(NULL);
    for (char *at=converted; at<converted+written; at=g_utf8_next_char(at)) {
        gunichar c=g_utf8_get_char(at);
        if (c==0x8a || c=='\n') g_string_append_c(out,'\n');
        else if (!g_unichar_iscntrl(c)) g_string_append_unichar(out,c);
    }
    g_free(converted); return g_string_free(out,FALSE);
}
static void copy_text(char *out, gsize capacity, const char *in) {
    g_strlcpy(out,in,capacity);
    char *end=NULL;
    if (!g_utf8_validate(out,-1,(const char **)&end)) *end=0;
}
static int bcd(guint8 n) { return (n>>4)>9 || (n&15)>9 ? -1 : (n>>4)*10+(n&15); }
static int duration(const guint8 *b) {
    int h=bcd(b[0]),m=bcd(b[1]),s=bcd(b[2]);
    return h<0 || m<0 || m>59 || s<0 || s>59 ? -1 : h*3600+m*60+s;
}
static gboolean event_read(TsInfoEvent *event, const guint8 *b, gsize n) {
    if (n<12) return FALSE;
    TsInfoEvent e={.valid=TRUE,.id=u16(b),.start=-1,.duration=duration(b+7)};
    int clock=duration(b+4);
    if (u16(b+2)!=0xffff && clock>=0 && clock<86400)
        e.start=((gint64)u16(b+2)-40587)*86400+clock;
    gsize end=12+(((b[10]&15)<<8)|b[11]);
    if (end>n) return FALSE;
    /* Pick the first advertised short-event language. Extended fragments must
     * match that language and are joined in descriptor-number order. */
    for (gsize at=12;at<end;) {
        if (at+2>end || at+2+b[at+1]>end) return FALSE;
        const guint8 *d=b+at+2; unsigned len=b[at+1];
        if (b[at]==0x4d && !e.language[0]) {
            if (len<5 || 5u+d[3]>len || 5u+d[3]+d[4+d[3]]!=len) return FALSE;
            memcpy(e.language,d,3);
            char *title=dvb_text(d+4,d[3]), *text=dvb_text(d+5+d[3],d[4+d[3]]);
            copy_text(e.title,sizeof(e.title),title); copy_text(e.text,sizeof(e.text),text);
            g_free(title); g_free(text);
        }
        at+=2+len;
    }
    GString *parts[16]={0};
    gboolean valid=TRUE;
    for (gsize at=12;at<end && valid;at+=2+b[at+1]) if (b[at]==0x4e) {
        const guint8 *d=b+at+2; unsigned len=b[at+1];
        if (len<6) { valid=FALSE; break; }
        if (!e.language[0]) memcpy(e.language,d+1,3);
        if (memcmp(e.language,d+1,3)) continue;
        unsigned part=d[0]>>4, items_end=5+d[4];
        if (part>(d[0]&15) || items_end>=len || items_end+1+d[items_end]!=len) { valid=FALSE; break; }
        if (parts[part]) g_string_free(parts[part],TRUE);
        parts[part]=g_string_new(NULL);
        for (unsigned i=5;i<items_end;) {
            unsigned a=d[i++];
            if (i+a>=items_end) { valid=FALSE; break; }
            char *key=dvb_text(d+i,a); i+=a;
            unsigned value_len=d[i++];
            if (i+value_len>items_end) { g_free(key); valid=FALSE; break; }
            char *value=dvb_text(d+i,value_len); i+=value_len;
            g_string_append_printf(parts[part],"%s: %s\n",key,value); g_free(key); g_free(value);
        }
        char *text=dvb_text(d+items_end+1,d[items_end]);
        g_string_append(parts[part],text); g_free(text);
    }
    GString *text=g_string_new(e.text);
    gboolean first=TRUE;
    for (unsigned i=0;i<16;i++) if (parts[i]) {
        if (first && text->len) g_string_append(text,"\n\n");
        first=FALSE; g_string_append(text,parts[i]->str); g_string_free(parts[i],TRUE);
    }
    copy_text(e.text,sizeof(e.text),text->str); g_string_free(text,TRUE);
    if (valid) *event=e;
    return valid;
}

static void pat_read(TsInfoParser *p, const guint8 *b, gsize n) {
    if (n<12 || (n-12)%4 || b[6]>b[7]) return;
    int version=(b[5]>>1)&31;
    guint16 tsid=u16(b+3);
    if (p->pat_version!=version || p->tsid!=tsid) {
        if (p->tsid!=tsid) { content_clear(p); p->state.count=0; p->state.selected=0; }
        p->tsid=tsid; p->pat_version=version; p->pending_count=0;
        memset(p->pat_sections,0,sizeof(p->pat_sections));
    }
    if (!p->pat_sections[b[6]]) {
        for (gsize at=8;at+4<=n-4;at+=4) if (u16(b+at) && p->pending_count<TS_INFO_SERVICES) {
            TsInfoService *s=&p->pending[p->pending_count++];
            memset(s,0,sizeof(*s)); s->id=u16(b+at); s->pmt_pid=u16(b+at+2)&0x1fff;
        }
        p->pat_sections[b[6]]=1;
    }
    for (unsigned i=0;i<=b[7];i++) if (!p->pat_sections[i]) return;
    gboolean found=FALSE;
    for (unsigned i=0;i<p->pending_count;i++) {
        for (unsigned j=0;j<p->state.count;j++) if (p->pending[i].id==p->state.services[j].id) {
            copy_text(p->pending[i].name,sizeof(p->pending[i].name),p->state.services[j].name);
            copy_text(p->pending[i].provider,sizeof(p->pending[i].provider),p->state.services[j].provider);
            if (p->pending[i].id==p->state.selected && p->pending[i].pmt_pid!=p->state.services[j].pmt_pid)
                content_clear(p);
        }
        if (p->pending[i].id==p->state.selected) found=TRUE;
    }
    memcpy(p->state.services,p->pending,p->pending_count*sizeof(TsInfoService));
    p->state.count=p->pending_count;
    if (!found) { content_clear(p); p->state.selected=p->state.count ? p->state.services[0].id : 0; }
}
static void sdt_read(TsInfoParser *p, const guint8 *b, gsize n) {
    if (n<15 || u16(b+3)!=p->tsid) return;
    for (gsize at=11;at+5<=n-4;) {
        guint16 sid=u16(b+at); gsize end=at+5+(u16(b+at+3)&0xfff);
        if (end>n-4) return;
        for (gsize d=at+5;d+2<=end;) {
            unsigned len=b[d+1]; if (d+2+len>end) return;
            const guint8 *v=b+d+2;
            if (b[d]==0x48 && len>=3 && 3u+v[1]<=len && 3u+v[1]+v[2+v[1]]<=len) {
                char *name=dvb_text(v+3+v[1],v[2+v[1]]);
                char *provider=dvb_text(v+2,v[1]);
                for (unsigned i=0;i<p->state.count;i++) if (p->state.services[i].id==sid) {
                    copy_text(p->state.services[i].name,sizeof(p->state.services[i].name),name);
                    copy_text(p->state.services[i].provider,sizeof(p->state.services[i].provider),provider);
                }
                g_free(name); g_free(provider);
            }
            d+=2+len;
        }
        at=end;
    }
}
static const char *stream_name(guint8 type) {
    switch (type) {
        case 0x01: return "MPEG-1 video";
        case 0x02: return "MPEG-2 video";
        case 0x03: return "MPEG-1 audio";
        case 0x04: return "MPEG-2 audio";
        case 0x05: return "Private sections";
        case 0x06: return "Private PES";
        case 0x0a: return "DSM-CC type A";
        case 0x0b: return "DSM-CC type B";
        case 0x0c: return "DSM-CC type C";
        case 0x0d: return "DSM-CC type D";
        case 0x0f: return "AAC";
        case 0x10: return "MPEG-4 video";
        case 0x11: return "AAC (LATM)";
        case 0x1b: return "H.264/AVC";
        case 0x24: return "H.265/HEVC";
        default: return "Other stream";
    }
}

/* Both descriptor loops have been bounds-checked before publication. */
static void describe_descriptors(GString *text, const guint8 *data, gsize length) {
    for (gsize i=0;i<length;i+=2+data[i+1]) {
        unsigned tag=data[i], size=data[i+1];
        const guint8 *value=data+i+2;
        g_string_append_printf(text,"    Descriptor 0x%02X",tag);
        if (tag==0x0a) {
            g_string_append(text," · ISO 639");
            for (unsigned j=0;j+4<=size;j+=4)
                if (g_ascii_isalpha(value[j]) && g_ascii_isalpha(value[j+1]) && g_ascii_isalpha(value[j+2]))
                    g_string_append_printf(text," · %.3s",(const char *)(value+j));
        } else if (tag==0x66 && size>=2) {
            guint16 id=u16(value);
            g_string_append_printf(text," · data_broadcast_id 0x%04X",id);
            if (id==6 || id==7) g_string_append(text,id==6 ? " (data carousel)" : " (object carousel)");
        } else if (tag==0x56) g_string_append(text," · Teletext");
        else if (tag==0x6a) g_string_append(text," · AC-3");
        else if (tag==0x7a) g_string_append(text," · E-AC-3");
        g_string_append(text,"   ");
        for (unsigned j=0;j<size;j++) g_string_append_printf(text," %02X",value[j]);
        g_string_append_c(text,'\n');
    }
}

char *ts_info_pmt_text(const TsInfoSnapshot *s) {
    if (!s->pmt_section) return g_strdup("Waiting for the selected program's PMT");
    gsize size;
    const guint8 *b=g_bytes_get_data(s->pmt_section,&size);
    GString *text=g_string_new(NULL);
    for (unsigned i=0;i<s->count;i++)
        if (s->services[i].id==s->selected)
            g_string_append_printf(text,"PMT PID 0x%04X    ",s->services[i].pmt_pid);
    g_string_append_printf(text,"Program %u    Version %u    PCR PID 0x%04X\n",
                           u16(b+3),(b[5]>>1)&31,u16(b+8)&0x1fff);
    gsize program_length=u16(b+10)&0xfff;
    describe_descriptors(text,b+12,program_length);
    for (gsize at=12+program_length;at<size-4;) {
        gsize length=u16(b+at+3)&0xfff;
        g_string_append_printf(text,"\nPID 0x%04X    Type 0x%02X    %s\n",
                               u16(b+at+1)&0x1fff,b[at],stream_name(b[at]));
        describe_descriptors(text,b+at+5,length);
        at+=5+length;
    }
    return g_string_free(text,FALSE);
}
static void pmt_read(TsInfoParser *p, const guint8 *b, gsize n) {
    if (n<16 || u16(b+3)!=p->state.selected || b[6] || b[7]) return;
    gsize at=12+(u16(b+10)&0xfff); guint16 pid=0x1fff; int best=0;
    const char *video="", *audio="";
    if (at>n-4) return;
    for (gsize d=12;d<at;) {
        if (d+2>at || d+2+b[d+1]>at) return;
        d+=2+b[d+1];
    }
    while (at<n-4) {
        if (at+5>n-4) return;
        gsize end=at+5+(u16(b+at+3)&0xfff); if (end>n-4) return;
        if (!*video) switch (b[at]) {
            case 0x01: video="MPEG-1"; break;
            case 0x02: video="MPEG-2"; break;
            case 0x10: video="MPEG-4"; break;
            case 0x1b: video="H.264/AVC"; break;
            case 0x24: video="H.265/HEVC"; break;
        }
        if (!*audio) switch (b[at]) {
            case 0x03: audio="MPEG-1 audio"; break;
            case 0x04: audio="MPEG-2 audio"; break;
            case 0x0f: audio="AAC"; break;
            case 0x11: audio="AAC (LATM)"; break;
        }
        for (gsize d=at+5;d<end;) {
            if (d+2>end || d+2+b[d+1]>end) return;
            if (b[at]==0x06 && b[d]==0x56 && b[d+1]>=5 && b[d+1]%5==0) {
                for (gsize entry=d+2;entry<d+2+b[d+1];entry+=5) {
                    unsigned type=b[entry+3]>>3;
                    if (type<1 || type>5) continue; /* reserved descriptor types */
                    int rank=type==1 ? 3 : type==3 || type==4 ? 2 : 1;
                    if (type==1 && (b[entry+3]&7)==1 && b[entry+4]==0) rank=4;
                    if (rank>best) { best=rank; pid=u16(b+at+1)&0x1fff; }
                }
            }
            d+=2+b[d+1];
        }
        at=end;
    }
    GBytes *previous=p->state.pmt_section;
    if (!previous || g_bytes_get_size(previous)!=n || memcmp(g_bytes_get_data(previous,NULL),b,n)) {
        g_clear_pointer(&p->state.pmt_section,g_bytes_unref);
        p->state.pmt_section=g_bytes_new(b,n);
    }
    if (pid!=p->ttx_pid) {
        teletext_clear(p); page_clear(&p->state);
        p->state.teletext_received=0;
    }
    p->ttx_pid=pid; p->state.pmt_seen=TRUE; p->state.teletext_announced=pid!=0x1fff;
    copy_text(p->state.video_codec,sizeof(p->state.video_codec),video);
    copy_text(p->state.audio_codec,sizeof(p->state.audio_codec),audio);
}
static void section_read(TsInfoParser *p, Assembly *a) {
    const guint8 *b=a->data; gsize n=a->used;
    if (n<12 || !(b[1]&0x80) || !(b[5]&1) || crc32(b,n)) return;
    if (a==&p->pat && b[0]==0) pat_read(p,b,n);
    else if (a==&p->sdt && b[0]==0x42) sdt_read(p,b,n);
    else if (a==&p->pmt && b[0]==2) pmt_read(p,b,n);
    else if (a==&p->eit && b[0]==0x4e && n>=18 && u16(b+3)==p->state.selected &&
             p->state.selected && u16(b+8)==p->tsid && b[6]<2 && b[6]<=b[7]) {
        TsInfoEvent event={0};
        if (n>18 && !event_read(&event,b+14,n-18)) return;
        int version=(b[5]>>1)&31;
        if (version!=p->eit_version) { memset(p->state.events,0,sizeof(p->state.events)); p->eit_version=version; }
        p->state.events[b[6]]=event;
        p->state.eit_received=g_get_real_time();
    }
}
static void section_bytes(TsInfoParser *p, Assembly *a, const guint8 *b, gsize n, gboolean new_sections) {
    while (n) {
        if (!a->used && (!new_sections || *b==0xff)) return;
        gsize need=a->used<3 ? 3-a->used : a->expected-a->used;
        gsize take=MIN(need,n); memcpy(a->data+a->used,b,take); a->used+=take; b+=take; n-=take;
        if (a->used==3) {
            a->expected=3+(u16(a->data+1)&0xfff);
            if (a->expected<12 || a->expected>4096) { a->used=a->expected=0; return; }
        }
        if (a->used==a->expected) {
            section_read(p,a); a->used=a->expected=0;
            if (!new_sections) return;
        }
    }
}
/* EN 300 706: render the broadcast 40 x 25 grid, including rows 0 and 24.
 * libzvbi's optional navigation generation is deliberately disabled: it may
 * replace the broadcaster's last row with decoder-generated page links.
 * Four immutable frames keep reveal/flash independent of packet arrival. */
static GBytes *teletext_frame(vbi_page *page, gboolean reveal, gboolean flash) {
    enum { WIDTH=40*12, HEIGHT=25*10 };
    guint8 indexed[WIDTH*HEIGHT];
    guint32 *pixels=g_new0(guint32,WIDTH*HEIGHT);
    vbi_draw_vt_page_region(page,VBI_PIXFMT_PAL8,indexed,WIDTH,0,0,40,25,reveal,flash);
    for (unsigned y=0;y<HEIGHT;y++) for (unsigned x=0;x<WIDTH;x++) {
        const vbi_char *cell=&page->text[(y/10)*page->columns+x/12];
        unsigned index=indexed[y*WIDTH+x];
        if (index>=G_N_ELEMENTS(page->color_map) || cell->opacity==VBI_TRANSPARENT_SPACE ||
            (cell->opacity==VBI_TRANSPARENT_FULL && index==cell->background)) continue;
        /* libzvbi palette is 0xAABBGGRR; Cairo RGB24 is native 0x00RRGGBB.
         * The separate TV text screen uses an opaque black video background. */
        guint32 color=page->color_map[index];
        if (!(color>>24)) continue;
        pixels[y*WIDTH+x]=((color&255)<<16)|(color&0xff00)|((color>>16)&255);
    }
    return g_bytes_new_take(pixels,WIDTH*HEIGHT*4);
}
static unsigned decimal_page(unsigned bcd) {
    unsigned tens=(bcd>>4)&15, ones=bcd&15;
    if (bcd<0x100 || bcd>0x899 || tens>9 || ones>9) return 0;
    return (bcd>>8)*100+tens*10+ones;
}

/* Fetch navigation separately: libzvbi may synthesize its own footer.
 * Only make cells clickable where its label matches the visible footer. */
static void collect_page_links(TsInfoSnapshot *s, vbi_page *broadcast) {
    vbi_page nav;
    if (!vbi_fetch_vt_page(broadcast->vbi,&nav,broadcast->pgno,broadcast->subno,
                           VBI_WST_LEVEL_3p5,25,TRUE)) return;
    for (int i=0;i<4;i++) s->color_pages[i]=decimal_page(nav.nav_link[i].pgno);
    for (int x=0;x<40;x++) {
        const vbi_char *cell=&nav.text[24*nav.columns+x];
        const vbi_char *visible=&broadcast->text[24*broadcast->columns+x];
        unsigned i=(unsigned char)nav.nav_index[x];
        if (cell->link && i<4 && cell->unicode==visible->unicode &&
            cell->foreground==visible->foreground)
            s->footer_links[x]=s->color_pages[i];
    }
    vbi_unref_page(&nav);
}
static void show_cached_page(TsInfoParser *p) {
    unsigned number=p->state.page_number;
    int bcd=((number/100)<<8)|(((number/10)%10)<<4)|(number%10);
    if (!p->latest[number-100].received) return;
    vbi_page page;
    if (!vbi_fetch_vt_page(p->teletext,&page,bcd,p->latest[number-100].subno,
                          VBI_WST_LEVEL_3p5,25,FALSE)) return;
    if (page.columns>=40 && page.rows>=25 && page.columns*page.rows<=(int)G_N_ELEMENTS(page.text)) {
        page_clear(&p->state);
        collect_page_links(&p->state,&page);
        p->state.page=teletext_frame(&page,FALSE,TRUE);
        p->state.page_flash_off=teletext_frame(&page,FALSE,FALSE);
        p->state.page_reveal=teletext_frame(&page,TRUE,TRUE);
        p->state.page_reveal_flash_off=teletext_frame(&page,TRUE,FALSE);
        p->state.width=480; p->state.height=250;
        p->state.teletext_received=p->latest[number-100].received;
    }
    vbi_unref_page(&page);
}
static void teletext_event(vbi_event *event, void *data) {
    TsInfoParser *p=data;
    if (event->type!=VBI_EVENT_TTX_PAGE) return;
    unsigned number=decimal_page(event->ev.ttx_page.pgno);
    if (!number) return;
    p->latest[number-100].subno=event->ev.ttx_page.subno;
    p->latest[number-100].received=g_get_real_time();
    if (number==p->state.page_number) show_cached_page(p);
}
static void teletext_open(TsInfoParser *p) {
    p->teletext=vbi_decoder_new();
    if (p->teletext && !vbi_event_handler_register(p->teletext,VBI_EVENT_TTX_PAGE,teletext_event,p))
        teletext_clear(p);
    if (p->teletext) vbi_teletext_set_level(p->teletext,VBI_WST_LEVEL_3p5);
    p->state.teletext_decoder=p->teletext!=NULL;
}
static void pes_read(TsInfoParser *p) {
    Assembly *a=&p->pes;
    /* EN 300 472 clauses 4.2-4.4: private_stream_1, fixed 45-byte
     * PES header, EBU identifier and length-delimited 44-byte data units.
     * Validate the whole PES before changing decoder state. */
    if (a->used<46 || memcmp(a->data,"\0\0\1\xbd",4) ||
        (a->data[6]&0xc0)!=0x80 || a->data[8]!=0x24 || a->used%184 ||
        a->data[45]<0x10 || a->data[45]>0x1f) return;
    for (gsize at=46;at<a->used;) {
        if (a->used-at<2) return;
        unsigned id=a->data[at], len=a->data[at+1];
        if (at+2+len>a->used) return;
        if ((id==2 || id==3) && (len!=44 || a->data[at+3]!=0xe4)) return;
        at+=2+len;
    }
    if (!p->teletext) teletext_open(p);
    if (!p->teletext) return;
    for (gsize at=46;at<a->used;at+=2+a->data[at+1]) {
        unsigned id=a->data[at];
        if (id!=2 && id!=3) continue; /* stuffing/reserved units are not text */
        vbi_sliced line={0};
        line.id=VBI_SLICED_TELETEXT_B;
        unsigned offset=a->data[at+2]&31;
        line.line=offset ? offset+((a->data[at+2]&32) ? 0 : 313) : 0;
        for (unsigned i=0;i<42;i++) line.data[i]=vbi_rev8(a->data[at+4+i]);
        /* This is a page viewer, not a timed subtitle overlay. A monotonic
         * decode clock also works for file replay fed faster than real time. */
        p->teletext_time+=0.04;
        vbi_decode(p->teletext,&line,1,p->teletext_time);
    }
}
static void packet_read(TsInfoParser *p, const guint8 *b) {
    unsigned pid=u16(b+1)&0x1fff; Assembly *a=NULL;
    if (pid==0) a=&p->pat;
    else if (pid==0x11) a=&p->sdt;
    else if (pid==0x12) a=&p->eit;
    else if (pid==p->ttx_pid && pid!=0x1fff) a=&p->pes;
    else for (unsigned i=0;i<p->state.count;i++)
        if (p->state.services[i].id==p->state.selected && p->state.services[i].pmt_pid==pid) a=&p->pmt;
    if (!a) return;
    unsigned afc=(b[3]>>4)&3, offset=4;
    if ((b[1]&0x80) || (b[3]&0xc0) || !afc) {
        assembly_clear(a); if (a==&p->pes) teletext_clear(p); return;
    }
    if (afc&2) {
        offset=5+b[4];
        if (offset>188) { assembly_clear(a); return; }
        if (b[4] && (b[5]&0x80)) { assembly_clear(a); if (a==&p->pes) teletext_clear(p); }
    }
    if (!(afc&1) || offset>=188) return;
    int cc=b[3]&15;
    if (cc==a->cc && !memcmp(a->previous,b,188)) return;
    if (a->cc>=0 && cc!=((a->cc+1)&15)) {
        assembly_clear(a); if (a==&p->pes) teletext_clear(p);
    }
    a->cc=cc; memcpy(a->previous,b,188);
    gboolean start=b[1]&0x40;
    const guint8 *payload=b+offset; gsize n=188-offset;
    if (a==&p->pes) {
        if (start) a->used=a->expected=0;
        else if (!a->used) return;
        if (a->used+n>sizeof(a->data)) { a->used=a->expected=0; return; }
        memcpy(a->data+a->used,payload,n); a->used+=n;
        if (!a->expected && a->used>=6) {
            a->expected=6+u16(a->data+4);
            if (a->expected<9) { a->used=a->expected=0; return; }
        }
        if (a->expected && a->used>=a->expected) {
            a->used=a->expected; pes_read(p); a->used=a->expected=0;
        }
    } else if (start) {
        unsigned pointer=*payload++; n--;
        if (pointer>n) { a->used=a->expected=0; return; }
        if (a->used) section_bytes(p,a,payload,pointer,FALSE);
        a->used=a->expected=0;
        section_bytes(p,a,payload+pointer,n-pointer,TRUE);
    } else if (a->used) section_bytes(p,a,payload,n,FALSE);
}
void ts_info_parser_feed(TsInfoParser *p, const guint8 *data, gsize size) {
    while (size) {
        unsigned take=MIN(size,sizeof(p->packet)-p->packet_used);
        memcpy(p->packet+p->packet_used,data,take); p->packet_used+=take; data+=take; size-=take;
        while (p->packet_used>=188) {
            if (p->packet[0]!=0x47 && p->synchronized) {
                assemblies_gap(p); p->synchronized=FALSE;
            }
            if (!p->synchronized) {
                if (p->packet[0]!=0x47) {
                    guint8 *sync=memchr(p->packet,0x47,p->packet_used);
                    unsigned skip=sync ? (unsigned)(sync-p->packet) : p->packet_used;
                    p->packet_used-=skip; memmove(p->packet,p->packet+skip,p->packet_used);
                    continue;
                }
                /* Require a second sync byte at the packet cadence when
                 * acquiring alignment (including after a queue overflow). */
                if (p->packet_used<189) break;
                if (p->packet[188]!=0x47) {
                    --p->packet_used; memmove(p->packet,p->packet+1,p->packet_used); continue;
                }
                p->synchronized=TRUE;
            }
            packet_read(p,p->packet); p->packet_used-=188;
            memmove(p->packet,p->packet+188,p->packet_used);
        }
    }
}

struct TsInfoBackend {
    GMutex mutex;
    GCond wake;
    GQueue queue;
    gsize queued;
    guint64 generation;
    guint16 selected;
    unsigned page_number;
    gboolean page_changed;
    gboolean reset, gap, eof, stop;
    gint done;
    GThread *thread;
    TsInfoSnapshot snapshot;
};
static void queue_clear(TsInfoBackend *b) {
    g_queue_clear_full(&b->queue,(GDestroyNotify)g_bytes_unref); b->queued=0;
}
static gpointer info_worker(gpointer data) {
    TsInfoBackend *b=data; TsInfoParser *p=ts_info_parser_new();
    while (TRUE) {
        g_mutex_lock(&b->mutex);
        while (!b->stop && !b->eof && !b->reset && !b->selected && !b->page_changed && g_queue_is_empty(&b->queue))
            g_cond_wait(&b->wake,&b->mutex);
        gboolean stop=b->stop, reset=b->reset, gap=b->gap;
        guint16 selected=b->selected; guint64 generation=b->generation;
        unsigned page_number=b->page_number;
        b->reset=b->gap=b->page_changed=FALSE; b->selected=0;
        GBytes *chunk=g_queue_pop_head(&b->queue);
        if (chunk) b->queued-=g_bytes_get_size(chunk);
        gboolean eof=b->eof && !chunk;
        g_mutex_unlock(&b->mutex);
        if (stop || eof) { if (chunk) g_bytes_unref(chunk); break; }
        if (reset) { ts_info_parser_free(p); p=ts_info_parser_new(); }
        else if (gap) ts_info_parser_gap(p);
        if (selected) ts_info_parser_select(p,selected);
        ts_info_parser_set_page(p,page_number);
        if (chunk) {
            gsize n; const guint8 *bytes=g_bytes_get_data(chunk,&n);
            ts_info_parser_feed(p,bytes,n); g_bytes_unref(chunk);
        }
        g_mutex_lock(&b->mutex);
        if (generation==b->generation && !b->stop) {
            ts_info_snapshot_clear(&b->snapshot); ts_info_parser_snapshot(p,&b->snapshot);
        }
        g_mutex_unlock(&b->mutex);
    }
    ts_info_parser_free(p); g_atomic_int_set(&b->done,TRUE); return NULL;
}
TsInfoBackend *ts_info_backend_new(void) {
    TsInfoBackend *b=g_new0(TsInfoBackend,1);
    g_mutex_init(&b->mutex); g_cond_init(&b->wake); g_queue_init(&b->queue);
    b->snapshot.teletext_decoder=TRUE;
    b->page_number=b->snapshot.page_number=100;
    b->thread=g_thread_new("ts-information",info_worker,b); return b;
}
void ts_info_backend_feed(TsInfoBackend *b, const guint8 *data, gsize size) {
    if (!b || !size) return;
    g_mutex_lock(&b->mutex);
    if (!b->stop && !b->eof) {
        if (b->queued+size>TS_INFO_QUEUE_LIMIT) { queue_clear(b); b->gap=TRUE; }
        if (size<=TS_INFO_QUEUE_LIMIT) { g_queue_push_tail(&b->queue,g_bytes_new(data,size)); b->queued+=size; }
        g_cond_signal(&b->wake);
    }
    g_mutex_unlock(&b->mutex);
}
void ts_info_backend_reset(TsInfoBackend *b) {
    if (!b) return;
    g_mutex_lock(&b->mutex);
    ++b->generation; b->reset=TRUE; b->selected=0; queue_clear(b);
    gboolean decoder=b->snapshot.teletext_decoder;
    ts_info_snapshot_clear(&b->snapshot); b->snapshot.teletext_decoder=decoder;
    b->page_number=b->snapshot.page_number=100; b->page_changed=FALSE;
    g_cond_signal(&b->wake); g_mutex_unlock(&b->mutex);
}
void ts_info_backend_select(TsInfoBackend *b, guint16 service) {
    if (!b || !service) return;
    g_mutex_lock(&b->mutex);
    gboolean found=FALSE;
    for (unsigned i=0;i<b->snapshot.count;i++) if (b->snapshot.services[i].id==service) found=TRUE;
    if (found && b->snapshot.selected!=service) {
        ++b->generation; b->selected=service; queue_clear(b); b->gap=TRUE;
        memset(b->snapshot.events,0,sizeof(b->snapshot.events));
        b->snapshot.eit_received=b->snapshot.teletext_received=0;
        page_clear(&b->snapshot);
        g_clear_pointer(&b->snapshot.pmt_section,g_bytes_unref);
        b->snapshot.pmt_seen=b->snapshot.teletext_announced=FALSE; b->snapshot.selected=service;
        g_cond_signal(&b->wake);
    }
    g_mutex_unlock(&b->mutex);
}
void ts_info_backend_snapshot(TsInfoBackend *b, TsInfoSnapshot *out) {
    if (!b) { memset(out,0,sizeof(*out)); return; }
    g_mutex_lock(&b->mutex); snapshot_copy(out,&b->snapshot); g_mutex_unlock(&b->mutex);
}
void ts_info_backend_set_page(TsInfoBackend *b, unsigned page) {
    if (!b || page<100 || page>899) return;
    g_mutex_lock(&b->mutex);
    if (!b->stop && b->page_number!=page) {
        ++b->generation; b->page_number=page; b->page_changed=TRUE;
        b->snapshot.page_number=page;
        page_clear(&b->snapshot); b->snapshot.teletext_received=0;
        /* Invalidate an in-flight old-page snapshot, without dropping TS or
         * resetting service/EIT/PES state. Commands also wake an idle worker. */
        g_cond_signal(&b->wake);
    }
    g_mutex_unlock(&b->mutex);
}
void ts_info_backend_finish(TsInfoBackend *b) {
    if (!b) return;
    g_mutex_lock(&b->mutex); b->eof=TRUE; g_cond_signal(&b->wake); g_mutex_unlock(&b->mutex);
}
void ts_info_backend_stop(TsInfoBackend *b) {
    if (!b) return;
    g_mutex_lock(&b->mutex); b->stop=TRUE; queue_clear(b); g_cond_signal(&b->wake); g_mutex_unlock(&b->mutex);
}
gboolean ts_info_backend_done(TsInfoBackend *b) { return !b || g_atomic_int_get(&b->done); }
void ts_info_backend_free(TsInfoBackend *b) {
    if (!b) return;
    ts_info_backend_stop(b); g_thread_join(b->thread); ts_info_snapshot_clear(&b->snapshot);
    g_cond_clear(&b->wake); g_mutex_clear(&b->mutex); g_free(b);
}
