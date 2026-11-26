#include "fmatv.h"
#include "analog/alignment.h"
#include "analog/pal_pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

_Static_assert(WIDTH == FMATV_WIDTH && HEIGHT == FMATV_HEIGHT, "field dimensions");
static void field(void *opaque, const uint32_t *pixels) {
    Decoder *d = opaque;
    unsigned char flags = 0;
    if (d->hsync_updates && d->fields-d->hsync_field <= 5 &&
        d->vsync_updates && d->tick-d->vsync_confirmed < d->rate*.1) flags |= FMATV_SYNC;
    if (d->pal.last_lock && d->tick-d->pal.last_lock < d->rate*.002) flags |= FMATV_BURST;
    if (fputc(flags, stdout) == EOF || fwrite(pixels, sizeof(*pixels), WIDTH*HEIGHT, stdout) != WIDTH*HEIGHT ||
        fflush(stdout)) exit(1);
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
    Decoder *d = calloc(1,sizeof(*d));
    IQ *iq = malloc(BLOCK*sizeof(*iq));
    int16_t *raw = malloc(BLOCK*2*sizeof(*raw));
    const double deviation = 4000000, guide_bw = fmin(3200000,bandwidth);
    int color = rate >= 12000000 && bandwidth >= 10000000;
    Alignment *video = alignment_create_video(rate,bandwidth,deviation,0,BLOCK,color);
    Alignment *guide = alignment_create(rate,guide_bw,deviation,0,BLOCK);
    PalPipeline *pal = color ? pal_pipeline_create() : NULL;
    int result = 1;
    if (!d || !iq || !raw || !video || !guide || alignment_set_deemphasis(video)) {
        fprintf(stderr,"[fm-atv] Cannot initialize PAL receiver\n");
        goto done;
    }
    alignment_set_dc_block(video,!strcmp(argv[4],"hackrf"));
    alignment_set_dc_block(guide,!strcmp(argv[4],"hackrf"));
    decoder_init(d,rate,deviation,0,field,d);
    d->free_sync = 1; /* Adaptive H/V clock, with no independent free-run preview. */
    d->guide_stride = (unsigned)fmax(1,floor(rate/fmax(4e6,2*guide_bw)));
    d->no_guide_raster = 1;
    d->video_deemphasis = 1;
    d->color = color;
    pal_init(&d->pal,rate);
    fprintf(stderr,"[fm-atv] FM0, adaptive sync, PAL de-emphasis, %d Hz video IF, burst detector %s\n",
            bandwidth,color ? "on" : "off (monochrome bandwidth)");
    size_t pending = 0, bytes = u8 ? 2 : 4;
    for (;;) {
        size_t n = fread(raw,1,BLOCK*bytes,stdin);
        if (ferror(stdin) || n%bytes) goto done;
        if (!n) break;
        n /= bytes;
        for (size_t i=0; i<n; ++i) {
            if (u8) {
                unsigned char *p = (unsigned char *)raw + 2*i;
                iq[i] = (IQ){(p[0]-128)/128.f,(p[1]-128)/128.f};
            } else iq[i] = (IQ){raw[2*i]/32768.f,raw[2*i+1]/32768.f};
        }
        if (alignment_queue(video,iq,n) || alignment_queue(guide,iq,n)) goto done;
        if (pending) pal_pipeline_decode(pal,d,alignment_previous(video,NULL,NULL),
                                         alignment_previous(guide,NULL,NULL),pending);
        pending = n;
    }
    if (pending) pal_pipeline_decode(pal,d,alignment_wait(video),alignment_wait(guide),pending);
    result = 0;
done:
    alignment_destroy(guide);
    alignment_destroy(video);
    pal_pipeline_destroy(pal);
    free(raw); free(iq); free(d);
    return result;
}
