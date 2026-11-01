#include "../src/iq_shift.h"
#include "../src/iq_bandfilter.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc != 5 && argc != 6) return 2;
    double rate=atof(argv[1]), offset=atof(argv[2]), width=atof(argv[3]);
    size_t block=strtoul(argv[4],NULL,10);
    if (!block || block>65536) return 2;
    IQShift shift;
    if (iq_shift_init(&shift,rate,offset) || !iq_shift_fits(rate,offset,width)) return 2;
    IQBandFilter *filter=iq_bandfilter_create(rate,width);
    if (!filter) return 2;
    int16_t *iq=malloc(block*4);
    if (!iq) return 1;
    float *work=malloc(block*2*sizeof(float));
    if (!work) return 1;
    int u8=argc==6 ? atoi(argv[5]) : 0;
    size_t n;
    while ((n=fread(iq,4,block,stdin))) {
        if (offset != 0) {
            for (size_t i=0;i<n*2;++i) work[i]=iq[i];
            iq_shift_process(&shift,work,n);
            iq_bandfilter_process_float(filter,work,n);
            iq_shift_pack(work,(unsigned char *)iq,n,u8);
        } else iq_bandfilter_process(filter,(unsigned char *)iq,n,0);
        if (fwrite(iq,u8 ? 2 : 4,n,stdout)!=n) return 1;
    }
    free(work); free(iq); iq_bandfilter_destroy(filter);
    return ferror(stdin) || fflush(stdout);
}
