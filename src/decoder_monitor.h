#ifndef DECODER_MONITOR_H
#define DECODER_MONITOR_H
#include <stdint.h>

/* Private, local IPC between workers and GUI from the same executable/build.
 * Independent of modulation: future backends publish the same two messages.
 * Separate messages let status continue during constellation/acquisition gaps. */
#define DECODER_MONITOR_MAGIC 0x4c444d31u
#define DECODER_POINTS 768
enum { DECODER_STATUS = 1, DECODER_CONSTELLATION = 2, DECODER_BUFFER = 3 };
enum { DECODER_OFDM = 1, DECODER_FEC = 2, DECODER_TS = 4, DECODER_SERVICE = 8 };
typedef struct {
    uint32_t magic, type;
    uint32_t locks, buffer_valid;
    float snr_db, pilot_lock, cfo_hz, rms_dbfs, peak_dbfs, buffer_percent;
    uint32_t packets, written_packets, rs_ok, rs_uncorrectable, cc_errors;
    char service[128], provider[128], modulation[24], fec[16], guard[16];
    /* Satellite-only values. Legacy DVB-T fields retain their meanings. */
    uint32_t satellite, constellation_order, lock_valid, fec_good, fec_bad;
    float mer_db;
    int32_t modcod, short_frame, pilots;
    /* DVB-T2 broadcast worker: cumulative live losses and selected PLP. */
    uint64_t input_dropped, fec_frames_dropped;
    int32_t plp_id;
} DecoderStatus;
typedef struct {
    uint32_t magic, type, count;
    float iq[DECODER_POINTS * 2];
} DecoderConstellation;
typedef struct {
    uint32_t magic, type, queued, capacity;
} DecoderBuffer;
#endif
