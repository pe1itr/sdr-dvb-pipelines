#include "../src/iq_bandfilter.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc != 5 && argc != 7) return 2;
    int u8 = atoi(argv[3]); size_t block = strtoul(argv[4], NULL, 10), bytes = u8 ? 2 : 4;
    if (!block || block > 65536) return 2;
    IQBandFilter *filter = iq_bandfilter_create(atof(argv[1]), atof(argv[2]));
    if (!filter) return 2;
    unsigned char *data = malloc(block*bytes);
    if (!data) return 1;
    size_t n, total = 0;
    while ((n = fread(data, 1, block*bytes, stdin))) {
        if (n % bytes) return 2;
        if (argc == 7 && total >= strtoul(argv[5], NULL, 10)) {
            if (iq_bandfilter_set(filter, atof(argv[6]))) return 2;
        }
        iq_bandfilter_process(filter, data, n/bytes, u8);
        if (fwrite(data, 1, n, stdout) != n) return 1;
        total += n/bytes;
    }
    free(data); iq_bandfilter_destroy(filter);
    return ferror(stdin) || fflush(stdout);
}
