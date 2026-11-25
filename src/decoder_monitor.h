#ifndef DECODER_MONITOR_H
#define DECODER_MONITOR_H
#include <stdint.h>

/* Private, local IPC between workers and GUI from the same executable/build.
 * Independent of modulation: future backends publish the same two messages.
 * Separate messages let status continue during constellation/acquisition gaps. */
#define DECODER_MONITOR_MAGIC 0x4c444d31u
#define DECODER_POINTS 768
enum { DECODER_STATUS = 1, DECODER_CONSTELLATION = 2, DECODER_BUFFER = 3, DECODER_DVBT_HEALTH = 4 };
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
    int32_t dvbt_bin_shift;
    /* T2 first-pass LDPC backend; GPU frames can still have CPU retries/BCH. */
    uint32_t ldpc_gpu_requested, ldpc_backend; /* 0 waiting/unknown, 1 CPU, 2 GPU */
    uint64_t ldpc_gpu_frames, ldpc_cpu_frames;
} DecoderStatus;
/* MT worker extends its RF status without changing the other workers' ABI. */
typedef struct {
    DecoderStatus rf;
    uint16_t agc1, agc2;
    uint32_t frequency_khz;
    float lna_gain_db, rf_input_dbm;
    int8_t bch_flag; /* -1 until DVB-S2 packet delineator lock */
} DecoderMtStatus;
typedef struct {
    uint32_t magic, type, count;
    float iq[DECODER_POINTS * 2];
} DecoderConstellation;
typedef struct {
    uint32_t magic, type, queued, capacity;
} DecoderBuffer;
typedef struct {
    uint32_t magic, type, chunks, rs_uncorrectable, written_packets;
    uint32_t symbols, weak_pilot_symbols, phase_repairs, frontend_discontinuities;
    uint32_t fifo_drop_symbols, low_pilot_drops, rs_corrected, cc_errors, transport_errors, sync_bad;
    uint64_t iq_dropped_samples_total, iq_producer_waits_total, outer_acquire_pending;
    char outer_state[24];
    double window_seconds, pilot_min, pilot_avg, snr_min, snr_avg;
} DecoderDvbtHealth;
#endif
