/* Build with the same flags as the production helper:
 * cc -O2 -Isrc dev/bench_bandfilter.c src/iq_bandfilter.c -lfftw3f -lm -o /tmp/bench-bandfilter
 * Usage: /tmp/bench-bandfilter [rate [bandwidth [seconds [u8]]]]
 * Measures filtering only, without SDR, decoder or pipe overhead. */
#define _POSIX_C_SOURCE 200809L
#include "iq_bandfilter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec+t.tv_nsec/1e9;
}
int main(int argc,char **argv) {
    unsigned rate=argc>1 ? strtoul(argv[1],NULL,10) : 20000000;
    double width=argc>2 ? strtod(argv[2],NULL) : 1778112;
    unsigned seconds=argc>3 ? strtoul(argv[3],NULL,10) : 3;
    int u8=argc>4 ? atoi(argv[4])!=0 : 0;
    if (argc>5 || !seconds || seconds>60 || rate<1 || rate>100000000 ||
        !iq_bandfilter_valid(rate,width)) return 2;
    double start=now();
    IQBandFilter *f=iq_bandfilter_create(rate,width);
    if (!f) return 1;
    double setup=now()-start;
    unsigned char original[65536*4],work[65536*4];
    uint32_t rng=92;
    size_t stride=u8 ? 2 : 4;
    for (size_t i=0;i<65536*stride;++i) {
        rng=1664525*rng+1013904223;original[i]=(unsigned char)(rng>>24);
    }
    uint64_t total=(uint64_t)rate*seconds;
    start=now();
    for (uint64_t done=0;done<total;) {
        size_t n=total-done<65536 ? (size_t)(total-done) : 65536;
        memcpy(work,original,n*stride);
        iq_bandfilter_process(f,work,n,u8);done+=n;
    }
    double elapsed=now()-start;
    printf("{\"rate\":%u,\"bandwidth\":%.0f,\"format\":\"%s\",\"signal_seconds\":%u,"
           "\"setup_seconds\":%.6f,\"processing_seconds\":%.6f,\"seconds_per_signal_second\":%.6f}\n",
           rate,width,u8 ? "u8" : "s16",seconds,setup,elapsed,elapsed/seconds);
    iq_bandfilter_destroy(f);return 0;
}
