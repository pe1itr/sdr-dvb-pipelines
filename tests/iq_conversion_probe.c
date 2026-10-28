/* Exercise exactly the conversion routines used in the hardware RX loop. */
#include "../src/iq_convert.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    int cs8 = !strcmp(argv[1], "cs8"), cs16 = !strcmp(argv[1], "cs16");
    int cf32 = !strcmp(argv[1], "cf32");
    if (cf32) {
        float input[4096]; int16_t output[4096]; size_t n;
        while ((n = fread(input, sizeof(float), 4096, stdin))) {
            if (n % 2) return 2;
            iq_cf32_to_s16(input, output, n/2);
            if (fwrite(output, sizeof(int16_t), n, stdout) != n) return 1;
        }
        return ferror(stdin) || fflush(stdout);
    }
    if (!cs8 && !cs16) return 2;
    int16_t input[4096]; unsigned char output[4096];
    size_t n;
    while ((n = fread(input, 1, sizeof(input), stdin))) {
        size_t sample_bytes = cs8 ? 2 : 4;
        if (n % sample_bytes) return 2;
        if (cs8) {
            iq_cs8_to_u8((unsigned char *)input, n/2);
            if (fwrite(input, 1, n, stdout) != n) return 1;
        } else {
            iq_cs16_to_u8(input, output, n/4);
            if (fwrite(output, 1, n/2, stdout) != n/2) return 1;
        }
    }
    return ferror(stdin) || fflush(stdout);
}
