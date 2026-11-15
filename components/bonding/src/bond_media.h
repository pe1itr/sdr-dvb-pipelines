#ifndef BOND_MEDIA_H
#define BOND_MEDIA_H
#include <stddef.h>
#include <stdint.h>
/* Internal, bounded single-program source PSI parser. */
typedef struct {
    uint8_t bytes[1024], previous[188];
    size_t count, need;
    unsigned cc;
    int seen;
} bond_media_section;
typedef struct {
    bond_media_section pat, pmt;
    uint8_t audio[8192];
    unsigned program, pmt_pid;
    uint8_t pat_table[1024], pmt_table[1024];
    size_t pat_length, pmt_length;
    int ready;
} bond_media;
int bond_media_route(bond_media *media, const uint8_t packet[188]);
void bond_media_null_packet(uint8_t packet[188], unsigned cc);
#endif
