#ifndef LINRAD_FMATV_H
#define LINRAD_FMATV_H
#define FMATV_NAME "FM-ATV"
#define FMATV_MIN_BANDWIDTH_HZ 250000
#define FMATV_WIDTH 640
#define FMATV_HEIGHT 288
/* The worker and display in the same executable exchange one native header
 * followed by WIDTH x HEIGHT RGB32 pixels per PAL field. */
#include <stdint.h>
enum {
    FMATV_SYNC=1, FMATV_BURST=2, FMATV_MONO=4, FMATV_NICAM=8,
    FMATV_AUDIO_702=16, FMATV_AUDIO_722=32, FMATV_AUDIO_738=64, FMATV_AUDIO_756=128,
    FMATV_STACK=256
};
typedef struct {
    uint16_t enabled, fm_mode;
    uint32_t detected;
    double snr_db, line_hz, raster_hz;
} FmatvFieldInfo;
unsigned fmatv_detectors(double rate, double bandwidth);
int fmatv_worker(int argc, char **argv);
#endif
