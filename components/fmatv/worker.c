#include "fmatv.h"
#include "analog/alignment.h"
#include "analog/pal_pipeline.h"
#include "analog/video_quality.h"
#include "analog/carrier_monitor.h"
#include "analog/sound.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

_Static_assert(WIDTH == FMATV_WIDTH && HEIGHT == FMATV_HEIGHT, "field dimensions");
typedef struct {
    Decoder *decoder;
    VideoQuality quality;
    CarrierMonitor *audio;
    unsigned enabled;
} Receiver;

static void field(void *opaque, const uint32_t *pixels) {
    Receiver *rx=opaque;
    Decoder *d=rx->decoder;
    int horizontal=d->hsync_updates && d->fields-d->hsync_field<=5 && d->free_period>0;
    int vertical=d->vsync_updates && d->tick-d->vsync_confirmed<d->rate*.1 && d->vsync_period>0;
    FmatvFieldInfo info={
        .enabled=rx->enabled,
        .detected=carrier_monitor_detected(rx->audio),
        .line_hz=horizontal ? d->rate/d->free_period : NAN,
        .raster_hz=vertical ? d->rate/d->vsync_period : NAN,
        .snr_db=horizontal && vertical && rx->quality.reading.valid ? rx->quality.reading.video_snr_db : NAN
    };
    if (horizontal && vertical) info.detected|=FMATV_SYNC;
    if (d->pal.last_lock && d->tick-d->pal.last_lock<d->rate*.002) info.detected|=FMATV_BURST;
    if (fwrite(&info,sizeof(info),1,stdout)!=1 || fwrite(pixels,sizeof(*pixels),WIDTH*HEIGHT,stdout)!=WIDTH*HEIGHT ||
        fflush(stdout)) exit(1);
}

static void decode(Receiver *rx, PalPipeline *pal,const float *video,const float *guide,size_t n) {
    Decoder *d=rx->decoder;
    uint64_t first=d->tick;
    pal_pipeline_decode(pal,d,video,guide,n);
    int synced=d->hsync_updates>=2 && d->vsync_updates && d->fields-d->hsync_field<=2 &&
        d->tick-d->vsync_confirmed<d->rate*.060;
    video_quality_process(&rx->quality,video,n,first,synced,d->free_origin,
        d->free_period>0 ? d->free_period : d->rate/d->line_hz,
        (double)d->field,d->vsync_period>0 ? d->vsync_period : d->rate/50);
}

unsigned fmatv_detectors(double rate,double bandwidth) {
    unsigned flags=FMATV_SYNC;
    if (rate>=12000000 && bandwidth>=10000000) flags|=FMATV_BURST;
    if (sound_enabled(rate,bandwidth)) flags|=FMATV_MONO|FMATV_NICAM;
    if (stereo_sound_enabled(rate,bandwidth)) flags|=FMATV_AUDIO_702|FMATV_AUDIO_722|FMATV_AUDIO_738|FMATV_AUDIO_756;
    return flags;
}

/* Return the next IF width at a block boundary, zero at EOF, or -1 on error. */
static int receive_band(int rate,int bandwidth,int u8,int hackrf,const char *control) {
    Decoder *d = calloc(1,sizeof(*d));
    IQ *iq = malloc(BLOCK*sizeof(*iq));
    int16_t *raw = malloc(BLOCK*2*sizeof(*raw));
    const double deviation = 4000000, guide_bw = fmin(3200000,bandwidth);
    unsigned enabled=fmatv_detectors(rate,bandwidth);
    int color = !!(enabled & FMATV_BURST);
    Alignment *video = alignment_create_video(rate,bandwidth,deviation,0,BLOCK,color);
    Alignment *guide = alignment_create(rate,guide_bw,deviation,0,BLOCK);
    PalPipeline *pal = color ? pal_pipeline_create() : NULL;
    Receiver rx={.decoder=d,.enabled=enabled};
    int result = -1;
    if (!d || !iq || !raw || !video || !guide || alignment_set_deemphasis(video)) {
        fprintf(stderr,"[fm-atv] Cannot initialize PAL receiver\n");
        goto done;
    }
    alignment_set_dc_block(video,hackrf);
    alignment_set_dc_block(guide,hackrf);
    if (enabled & FMATV_MONO) {
        int stereo=!!(enabled & FMATV_AUDIO_702);
        rx.audio=carrier_monitor_create(rate,stereo);
        if (!rx.audio) { fprintf(stderr,"[fm-atv] Cannot start audio carrier monitor\n"); goto done; }
        alignment_set_consumer(video,carrier_monitor_feed,rx.audio);
    }
    video_quality_init(&rx.quality,rate);
    decoder_init(d,rate,deviation,0,field,&rx);
    d->free_sync = 1; /* Adaptive H/V clock, with no independent free-run preview. */
    d->guide_stride = (unsigned)fmax(1,floor(rate/fmax(4e6,2*guide_bw)));
    d->no_guide_raster = 1;
    d->video_deemphasis = 1;
    d->color = color;
    pal_init(&d->pal,rate);
    fprintf(stderr,"[fm-atv] FM0, adaptive sync, PAL de-emphasis, %d Hz video IF, burst detector %s\n",
            bandwidth,color ? "on" : "off (monochrome bandwidth)");
    size_t pending = 0, bytes = u8 ? 2 : 4;
    size_t until_check=rate/10;
    for (;;) {
        size_t n = fread(raw,1,BLOCK*bytes,stdin);
        if (ferror(stdin) || n%bytes) goto done;
        if (!n) { result=0; break; }
        n /= bytes;
        for (size_t i=0; i<n; ++i) {
            if (u8) {
                unsigned char *p = (unsigned char *)raw + 2*i;
                iq[i] = (IQ){(p[0]-128)/128.f,(p[1]-128)/128.f};
            } else iq[i] = (IQ){raw[2*i]/32768.f,raw[2*i+1]/32768.f};
        }
        if (alignment_queue(video,iq,n) || alignment_queue(guide,iq,n)) goto done;
        if (pending) decode(&rx,pal,alignment_previous(video,NULL,NULL),
                                         alignment_previous(guide,NULL,NULL),pending);
        pending = n;
        if (control && n>=until_check) {
            char *contents=NULL;
            until_check=rate/10;
            if (g_file_get_contents(control,&contents,NULL,NULL)) {
                gint64 next=0;
                gboolean valid=g_ascii_string_to_signed(contents,10,FMATV_MIN_BANDWIDTH_HZ,rate,&next,NULL);
                g_free(contents);
                if (valid && next!=bandwidth) {
                    result=(int)next;
                    break;
                }
            }
        } else if (control) until_check-=n;
    }
    if (pending) decode(&rx,pal,alignment_wait(video),alignment_wait(guide),pending);
done:
    alignment_destroy(guide);
    alignment_destroy(video);
    carrier_monitor_destroy(rx.audio);
    pal_pipeline_destroy(pal);
    free(raw); free(iq); free(d);
    return result;
}

int fmatv_worker(int argc, char **argv) {
    if (argc != 5) return 2;
    int rate = atoi(argv[1]), bandwidth = atoi(argv[2]);
    int u8 = !strcmp(argv[3], "u8");
    if (rate < 5000000 || rate > 64000000 || bandwidth < FMATV_MIN_BANDWIDTH_HZ || bandwidth > rate ||
        (!u8 && strcmp(argv[3],"s16"))) return 2;
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const char *control=g_getenv("LINRAD_FMATV_CONTROL");
    do {
        bandwidth=receive_band(rate,bandwidth,u8,!strcmp(argv[4],"hackrf"),control);
    } while (bandwidth>0);
    return bandwidth<0;
}
