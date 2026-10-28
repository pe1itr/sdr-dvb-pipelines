#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/iq_monitor.h"
int main(int argc, char **argv) {
    unsigned char data[4096];
    double rate = argc > 1 ? atof(argv[1]) : 158730;
    size_t n;
    while ((n=fread(data,4,1024,stdin))) {
        iq_monitor_feed(data,n,0,rate);
        if(fwrite(data,4,n,stdout)!=n) return 1;
    }
    return ferror(stdin) ? 1 : 0;
}
