#ifndef DVB_BOND_H
#define DVB_BOND_H

/* Stable integration ABI v1. Opaque handles; all media ports are 188-byte TS.
 * No threads, sockets, clocks, RF devices, callbacks or allocation in push/pop.
 * Serialize calls to each handle. Rebuild/link compatible v1 releases without
 * changing the transmitter/receiver TS plumbing. See docs/integration-v1.md. */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DVB_BOND_API_V1 1u
#define DVB_BOND_TS_SIZE 188u
#define DVB_BOND_PROFILE_XOR8 1u
#define DVB_BOND_PROFILE_XOR16 2u
#define DVB_BOND_PROFILE_HYBRID16 3u
#define DVB_BOND_EVENT_TUNE_PARTNER 1u
#define DVB_BOND_EVENT_PAIR_CONFIRMED 2u
#define DVB_BOND_EVENT_END 3u
#define DVB_BOND_WAIT 0
#define DVB_BOND_PACKET 1
#define DVB_BOND_END 2
#define DVB_BOND_INVALID (-1)

typedef struct dvb_bond_tx dvb_bond_tx;
typedef struct dvb_bond_rx dvb_bond_rx;

typedef struct {
    uint32_t struct_size, api_version;
    uint64_t bond_id, session, frequency_hz[2];
    uint32_t source_bitrate, lane_bitrate, profile, interleave_groups;
    uint32_t announcement_ms, tx_queue_frames, rx_window_groups, revision;
    uint64_t playout_delay_ns;
    uint64_t reserved[8];
} dvb_bond_config_v1;

typedef struct {
    uint32_t struct_size, api_version;
    uint32_t type, receiver_slot, lane, rf_profile;
    uint64_t sequence, bond_id, session, frequency_hz, anchor_frequency_hz;
    int64_t partner_offset_hz;
    uint32_t bandwidth_hz, source_bitrate, lane_bitrate, profile;
    uint32_t interleave_groups, guard_denominator, fec_numerator, fec_denominator;
    uint64_t reserved[4];
} dvb_bond_event_v1;

typedef struct {
    uint32_t struct_size, api_version;
    uint32_t discovered_mask, pair_verified, active, ended;
    uint64_t bond_id, session, frequency_hz[2], next_source_packet, total_source_packets;
    uint32_t source_bitrate, lane_bitrate, profile, interleave_groups;
    uint64_t input_packets[2], output_packets[2], queue_drops[2];
    uint64_t malformed, continuity_errors, recovered_symbols, missing_packets, late_frames;
    uint64_t tune_request_sequence, tune_frequency_hz;
    int64_t tune_offset_hz;
    uint32_t tune_receiver_slot, tune_lane;
    uint64_t event_overflows, reserved[8];
} dvb_bond_status_v1;

/* Defaults: QPSK/2K/40 kHz/FEC 1/2/GI 1/32, 50 kHz spacing, 50,100 bit/s
 * source, 30,160 bit/s per outer TS, XOR16, one group, 2s announcements.
 * Change session to a new nonzero value for every transmitter restart. */
void dvb_bond_config_init_v1(dvb_bond_config_v1 *config);
uint32_t dvb_bond_api_version(void);
/* Conservative steady-state ceiling including 15 outer TS per 2 DBND frames
 * and one PAT + one PMT per announcement. 0 means invalid configuration. */
uint32_t dvb_bond_source_limit_v1(const dvb_bond_config_v1 *config);

dvb_bond_tx *dvb_bond_tx_create_v1(const dvb_bond_config_v1 *config);
void dvb_bond_tx_destroy(dvb_bond_tx *tx);
int dvb_bond_tx_push(dvb_bond_tx *tx, const uint8_t packet[DVB_BOND_TS_SIZE]);
/* Optional byte-stream adapter. First byte must start a TS packet; partial
 * packets are retained across calls. Do not mix push/write with a partial. */
int dvb_bond_tx_write(dvb_bond_tx *tx, const uint8_t *bytes, size_t length);
int dvb_bond_tx_finish(dvb_bond_tx *tx);
/* Pull independently for lanes 0/1. WAIT until the next lane packet tick;
 * PACKET includes idle nulls; END after finish, queue drain and two EOS copies.
 * now_ns is a caller-owned monotonic clock (real or virtual). */
int dvb_bond_tx_pop(dvb_bond_tx *tx, unsigned lane, uint64_t now_ns,
                     uint8_t packet[DVB_BOND_TS_SIZE]);
uint64_t dvb_bond_tx_next_due_ns(const dvb_bond_tx *tx, unsigned lane);
int dvb_bond_tx_status(const dvb_bond_tx *tx, dvb_bond_status_v1 *status);

/* RX settings for RF discovery are learned from PMT. Config provides buffer
 * bounds and playout delay; session=0 accepts a discovered session. */
dvb_bond_rx *dvb_bond_rx_create_v1(const dvb_bond_config_v1 *config);
void dvb_bond_rx_destroy(dvb_bond_rx *rx);
int dvb_bond_rx_reset(dvb_bond_rx *rx);
int dvb_bond_rx_push(dvb_bond_rx *rx, unsigned receiver_slot,
                      const uint8_t packet[DVB_BOND_TS_SIZE], uint64_t now_ns);
int dvb_bond_rx_write(dvb_bond_rx *rx, unsigned receiver_slot,
                       const uint8_t *bytes, size_t length, uint64_t now_ns);
/* Joins at first CRC-valid received group. Playout starts delay_ns after that
 * frame, with no wait for the partner. Missing packets become nulls on time. */
int dvb_bond_rx_pop(dvb_bond_rx *rx, uint64_t now_ns,
                     uint8_t packet[DVB_BOND_TS_SIZE]);
uint64_t dvb_bond_rx_next_due_ns(const dvb_bond_rx *rx);
/* Returns 1 for an event, 0 if empty, -1 for invalid args. Events are one-shot
 * per discovery generation; the host owns receiver startup and tuning. */
int dvb_bond_rx_poll_event(dvb_bond_rx *rx, dvb_bond_event_v1 *event);
int dvb_bond_rx_status(const dvb_bond_rx *rx, dvb_bond_status_v1 *status);
/* Same information for a host-owned endpoint; NUL-terminated JSON schema v1.
 * Returns bytes excluding NUL, or -1 if the buffer is too small. */
int dvb_bond_rx_status_json(const dvb_bond_rx *rx, char *buffer, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
