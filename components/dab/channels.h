#ifndef DAB_CHANNELS_H
#define DAB_CHANNELS_H
#include <stddef.h>
#include <string.h>
typedef struct { const char *name; unsigned hz; } DabChannel;
static const DabChannel dab_channels[] = {
    {"5A",174928000},{"5B",176640000},{"5C",178352000},{"5D",180064000},
    {"6A",181936000},{"6B",183648000},{"6C",185360000},{"6D",187072000},
    {"7A",188928000},{"7B",190640000},{"7C",192352000},{"7D",194064000},
    {"8A",195936000},{"8B",197648000},{"8C",199360000},{"8D",201072000},
    {"9A",202928000},{"9B",204640000},{"9C",206352000},{"9D",208064000},
    {"10A",209936000},{"10B",211648000},{"10C",213360000},{"10D",215072000},
    {"11A",216928000},{"11B",218640000},{"11C",220352000},{"11D",222064000},
    {"12A",223936000},{"12B",225648000},{"12C",227360000},{"12D",229072000},
    {"13A",230784000},{"13B",232496000},{"13C",234208000},{"13D",235776000},
    {"13E",237488000},{"13F",239200000}
};
#define DAB_CHANNEL_COUNT (sizeof(dab_channels)/sizeof(dab_channels[0]))
static inline int dab_channel_index(const char *name) {
    for (size_t i=0; i<DAB_CHANNEL_COUNT; ++i)
        if (name && !strcmp(name,dab_channels[i].name)) return (int)i;
    return -1;
}
static inline int dab_channel_nearest(double hz) {
    size_t best=0;
    for (size_t i=1; i<DAB_CHANNEL_COUNT; ++i) {
        double a=hz-dab_channels[best].hz, b=hz-dab_channels[i].hz;
        if (b*b<a*a) best=i;
    }
    return (int)best;
}
#endif
