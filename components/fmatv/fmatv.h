#ifndef LINRAD_FMATV_H
#define LINRAD_FMATV_H
#define FMATV_NAME "FM-ATV"
#define FMATV_MIN_BANDWIDTH_HZ 250000
#define FMATV_WIDTH 640
#define FMATV_HEIGHT 288
/* Each worker output field is one flags byte followed by native RGB32 pixels. */
enum { FMATV_SYNC = 1, FMATV_BURST = 2 };
int fmatv_worker(int argc, char **argv);
#endif
