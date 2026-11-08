/* Exercise the driver without connected hardware, including partial setup failures. */
#include "pluto_rx.h"
#include <iio.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
struct iio_context { int unused; };
struct iio_device { int id; };
struct iio_channel { int id; };
struct iio_buffer { int16_t data[32]; };
struct iio_scan_context { int unused; };
struct iio_context_info { const char *uri; };
static struct iio_context ctx;
static struct iio_device phy={0},rx={1};
static struct iio_channel channels[4]={{0},{1},{2},{3}};
static struct iio_buffer buffer;
static struct iio_scan_context scan;
static struct iio_context_info infos[2]={{"usb:1"},{"usb:2"}};
static struct iio_context_info *info_list[3]={&infos[0],&infos[1],NULL};
static struct iio_data_format fmt;
static int contexts, buffers, enabled, calls, fail_at, scan_count=1, missing;
static ssize_t refill_result;
static size_t capacity;
static long long rate,bandwidth,frequency;
static double gain;
static int check(void) { return ++calls==fail_at?-EIO:0; }
struct iio_context *iio_create_context_from_uri(const char *uri) {
    assert(!strncmp(uri,"usb:",4) || !strncmp(uri,"ip:",3)); ++contexts; return &ctx;
}
void iio_context_destroy(struct iio_context *c) { assert(c==&ctx); --contexts; }
struct iio_scan_context *iio_create_scan_context(const char *backend,unsigned int flags) {
    assert(!strcmp(backend,"usb") && !flags); return &scan;
}
ssize_t iio_scan_context_get_info_list(struct iio_scan_context *s,struct iio_context_info ***list) {
    assert(s==&scan); *list=info_list; return scan_count;
}
const char *iio_context_info_get_uri(const struct iio_context_info *i) { return i->uri; }
void iio_context_info_list_free(struct iio_context_info **list) { assert(list==info_list); }
void iio_scan_context_destroy(struct iio_scan_context *s) { assert(s==&scan); }
int iio_context_set_timeout(struct iio_context *c,unsigned int ms) { assert(c==&ctx && (ms==1000 || ms==5000)); return check(); }
struct iio_device *iio_context_find_device(const struct iio_context *c,const char *name) {
    assert(c==&ctx); if(missing) return NULL;
    if(!strcmp(name,"ad9361-phy")) return &phy;
    assert(!strcmp(name,"cf-ad9361-lpc")); return &rx;
}
struct iio_channel *iio_device_find_channel(const struct iio_device *d,const char *name,bool output) {
    if(d==&phy) {
        if(!strcmp(name,"altvoltage0")) { assert(output); return &channels[1]; }
        assert(!strcmp(name,"voltage0") && !output); return &channels[0];
    }
    assert(d==&rx && !output);
    return &channels[!strcmp(name,"voltage0")?2:3];
}
ssize_t iio_channel_attr_write(const struct iio_channel *c,const char *a,const char *v) {
    assert(c==&channels[0]);
    assert((!strcmp(a,"rf_port_select") && !strcmp(v,"A_BALANCED")) ||
           (!strcmp(a,"gain_control_mode") && !strcmp(v,"manual")));
    int rc=check(); return rc?rc:(ssize_t)strlen(v);
}
int iio_channel_attr_write_longlong(const struct iio_channel *c,const char *a,long long v) {
    if(!strcmp(a,"frequency")) { assert(c==&channels[1]); frequency=v; }
    else if(!strcmp(a,"sampling_frequency")) { assert(c==&channels[0]); rate=v; }
    else { assert(c==&channels[0] && !strcmp(a,"rf_bandwidth")); bandwidth=v; }
    return check();
}
int iio_channel_attr_read_longlong(const struct iio_channel *c,const char *a,long long *v) {
    (void)c; *v=!strcmp(a,"frequency")?frequency:!strcmp(a,"sampling_frequency")?rate:bandwidth; return check();
}
int iio_channel_attr_write_double(const struct iio_channel *c,const char *a,double v) {
    assert(c==&channels[0] && !strcmp(a,"hardwaregain")); gain=v; return check();
}
int iio_channel_attr_read_double(const struct iio_channel *c,const char *a,double *v) {
    assert(c==&channels[0] && !strcmp(a,"hardwaregain")); *v=gain; return check();
}
unsigned int iio_device_get_channels_count(const struct iio_device *d) { assert(d==&rx); return 2; }
struct iio_channel *iio_device_get_channel(const struct iio_device *d,unsigned int k) { assert(d==&rx && k<2); return &channels[k+2]; }
bool iio_channel_is_scan_element(const struct iio_channel *c) { return c->id>=2; }
void iio_channel_enable(struct iio_channel *c) { enabled|=1<<c->id; }
void iio_channel_disable(struct iio_channel *c) { enabled&=~(1<<c->id); }
const struct iio_data_format *iio_channel_get_data_format(const struct iio_channel *c) { (void)c; return &fmt; }
int iio_device_set_kernel_buffers_count(const struct iio_device *d,unsigned int n) { assert(d==&rx && n==4); return check(); }
struct iio_buffer *iio_device_create_buffer(const struct iio_device *d,size_t n,bool cyclic) {
    assert(d==&rx && !cyclic && n<=16 && !buffers); if(check()) { errno=EIO; return NULL; }
    capacity=n; ++buffers; return &buffer;
}
void iio_buffer_destroy(struct iio_buffer *b) { assert(b==&buffer && buffers==1); --buffers; }
ssize_t iio_buffer_refill(struct iio_buffer *b) { assert(b==&buffer); return refill_result; }
ptrdiff_t iio_buffer_step(const struct iio_buffer *b) { (void)b; return 4; }
void *iio_buffer_first(const struct iio_buffer *b,const struct iio_channel *c) { return (void *)&b->data[c->id-2]; }
void *iio_buffer_end(const struct iio_buffer *b) { return (void *)&b->data[2*capacity]; }
void iio_channel_convert(const struct iio_channel *c,void *dst,const void *src) {
    (void)c; const unsigned char *p=src;
    unsigned v=fmt.is_be?((unsigned)p[0]<<8)|p[1]:((unsigned)p[1]<<8)|p[0];
    v=(v>>fmt.shift)&((1u<<fmt.bits)-1);
    int16_t signed_v=(int16_t)((int)(v^(1u<<(fmt.bits-1)))-(int)(1u<<(fmt.bits-1)));
    memcpy(dst,&signed_v,2);
}
static PlutoConfig config(void) {
    return (PlutoConfig){.ref="internal",.rate=10e6,.bandwidth=8e6,.frequency=1252e6,.gain=30};
}
static void clean(void) { assert(!contexts && !buffers && !enabled); }
static void test_sample_formats(void)
{
    /* Exhaust every 16-bit wire word, including undefined padding bits,
     * for both byte orders and every supported 12-bit shift. */
    for(unsigned bits=12;bits<=16;bits+=4) for(unsigned shift=0;shift<=16-bits;++shift)
    for(int be=0;be<2;++be) {
        fmt=(struct iio_data_format){.length=16,.bits=bits,.shift=shift,.is_be=be,.is_signed=true,.repeat=1};
        PlutoConfig c=config(); PlutoSource *s=pluto_source_open(&c); assert(s);
        PlutoIQ iq[4]; refill_result=16;
        /* Allocate buffer before filling its wire bytes. */
        assert(pluto_source_read(s,iq,4)==4);
        for(unsigned word=0;word<65536;word+=4) {
            unsigned char *p=(unsigned char *)buffer.data;
            int16_t expected[8];
            for(unsigned k=0;k<8;++k) {
                unsigned v=k%2?65535-(word+k/2):word+k/2;
                p[2*k]=be?v>>8:v; p[2*k+1]=be?v:v>>8;
                iio_channel_convert(&channels[2+k%2],&expected[k],p+2*k);
            }
            assert(pluto_source_read(s,iq,4)==4);
            for(unsigned k=0;k<4;++k) {
                assert(iq[k].i==expected[2*k]/(float)(1u<<(bits-1)));
                assert(iq[k].q==expected[2*k+1]/(float)(1u<<(bits-1)));
            }
        }
        pluto_source_close(s); clean();
    }
}
int main(void) {
    fmt=(struct iio_data_format){.length=16,.bits=12,.is_signed=true,.repeat=1};
    PlutoConfig c=config();
    PlutoSource *s=pluto_source_open(&c); assert(s); int setup_calls=calls;
    assert(c.rate==10e6 && c.bandwidth==8e6 && c.frequency==1252e6 && c.gain==30);
    PlutoIQ iq[4]; buffer.data[0]=2047; buffer.data[1]=0x800; buffer.data[2]=1024; buffer.data[3]=0xc00;
    refill_result=8; assert(pluto_source_read(s,iq,4)==2);
    assert(iq[0].i==2047/2048.0f && iq[0].q==-1 && iq[1].i==.5f && iq[1].q==-.5f);
    double hz=1300e6,g=40; assert(!pluto_source_tune(s,&hz,&g)); assert(!buffers && hz==1300e6 && g==40);
    /* Saved gain and arrow keys cannot exceed the current RF band's range. */
    const double frequencies[]={1252e6,1300e6,1300e6+2,4000e6,4000e6+2,5800e6};
    const double maxima[]={73,73,71,71,62,62};
    for(size_t k=0;k<sizeof(frequencies)/sizeof(*frequencies);++k) {
        hz=frequencies[k]; g=100;
        assert(!pluto_source_tune(s,&hz,&g) && g==maxima[k] && gain==g);
        g+=1; assert(!pluto_source_tune(s,&hz,&g) && g==maxima[k]);
        g-=1; assert(!pluto_source_tune(s,&hz,&g) && g==maxima[k]-1);
    }
    g=-1; assert(!pluto_source_tune(s,&hz,&g) && g==0);
    refill_result=-ETIMEDOUT; assert(pluto_source_read(s,iq,4)==0);
    refill_result=-EOVERFLOW; assert(pluto_source_read(s,iq,4)==-2);
    refill_result=-EIO; assert(pluto_source_read(s,iq,4)==-1);
    refill_result=3; assert(pluto_source_read(s,iq,4)==-1);
    refill_result=20; assert(pluto_source_read(s,iq,4)==-1);
    pluto_source_close(s); clean();
    for(int k=1;k<=setup_calls;++k) {
        calls=0; fail_at=k; c=config(); assert(!pluto_source_open(&c)); clean();
    }
    fail_at=0; calls=0; scan_count=0; c=config(); assert(!pluto_source_open(&c)); clean();
    scan_count=2; assert(!pluto_source_open(&c)); clean(); scan_count=1;
    c=config(); c.gain=100; s=pluto_source_open(&c);
    assert(s && c.gain==73); pluto_source_close(s); clean();
    c.pluto_uri="ip:192.168.2.1"; missing=1; assert(!pluto_source_open(&c)); clean(); missing=0;
    fmt.bits=8; assert(!pluto_source_open(&c)); clean();
    fmt.bits=12; fmt.shift=4; fmt.is_be=true;
    s=pluto_source_open(&c); assert(s);
    unsigned char *p=(unsigned char *)buffer.data; p[0]=0x80; p[1]=0; p[2]=0x40; p[3]=0;
    refill_result=4; assert(pluto_source_read(s,iq,4)==1); assert(iq[0].i==-1 && iq[0].q==.5f);
    pluto_source_stop(s); assert(!buffers);
    fail_at=calls+1; assert(pluto_source_read(s,iq,4)==-1); fail_at=0;
    pluto_source_close(s); clean();
    fmt=(struct iio_data_format){.length=16,.bits=16,.is_signed=true,.is_fully_defined=true,.repeat=1};
    const uint16_t endian=1; fmt.is_be=*(const unsigned char *)&endian==0;
    s=pluto_source_open(&c); assert(s); buffer.data[0]=-32768; buffer.data[1]=16384;
    assert(pluto_source_read(s,iq,4)==1 && iq[0].i==-1 && iq[0].q==.5f);
    pluto_source_close(s); clean();
    c.channel=1; assert(!pluto_source_open(&c)); c.channel=0; c.ref="external"; assert(!pluto_source_open(&c)); clean();
    test_sample_formats();
    puts("Pluto RX tests passed");
}
