/*
 * This file is part of DVB channel bonding.
 * Copyright (C) 2026 Rob Hardenberg, PE1ITR
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * DVB channel bonding is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License.
 *
 * DVB channel bonding is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with DVB channel bonding. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef DVB_BOND_DISCOVERY_H
#define DVB_BOND_DISCOVERY_H

#include "bonding.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOND_ANNOUNCEMENT_SIZE 64u
#define BOND_DISCOVERY_DESCRIPTOR_TAG 0x80u
#define BOND_DISCOVERY_DESCRIPTOR_SIZE (BOND_ANNOUNCEMENT_SIZE + 2u)
#define BOND_RF_DVBT_NARROW_QPSK_2K_1_2_GI_1_32 1u

/* Experimental protocol, not a DVB descriptor. Each lane advertises the whole
 * pair without needing the partner or the reconstructed MPEG-TS. */
typedef struct {
    uint64_t bond_id, session;
    uint32_t revision;
    unsigned lane, group_size;
    uint64_t own_frequency_hz, partner_frequency_hz;
    uint32_t bandwidth_hz, source_bitrate, interval_ms;
    unsigned rf_profile;
} bond_announcement;

/* Frequencies are positive integer Hz, at most INT64_MAX; interval 100..60000
 * ms. Group/FEC/bundle and RF profile must be supported. Returns 0 or -1.
 * A failed decode leaves the caller's announcement unchanged. */
int bond_announcement_encode(const bond_announcement *announcement,
                             uint8_t wire[BOND_ANNOUNCEMENT_SIZE]);
int bond_announcement_decode(bond_announcement *announcement,
                             const uint8_t *wire, size_t length);
/* Experimental private PMT descriptor: tag 0x80, length 64, DBNA payload.
 * Tag 0x80 is not globally assigned to this project: also validate DBNA/version.
 * The outer TS mux/parser owns PAT/PMT, section CRC, reassembly and repetition.
 * These helpers only encode/decode a descriptor including its two-byte header. */
int bond_announcement_descriptor_encode(const bond_announcement *announcement,
    uint8_t descriptor[BOND_DISCOVERY_DESCRIPTOR_SIZE]);
int bond_announcement_descriptor_decode(bond_announcement *announcement,
                                        const uint8_t *descriptor, size_t length);
/* Returns 1 only for a fully matching reciprocal pair, otherwise 0. */
int bond_announcement_pair(const bond_announcement *a,
                           const bond_announcement *b);
/* Nominal partner minus nominal own frequency, not an AFC correction.
 * Only call on a successfully validated announcement. */
int64_t bond_announcement_offset_hz(const bond_announcement *announcement);

/* Request a second receiver using the peer's complete nominal tuning record.
 * The application implements hardware tuning and checks its supported range. */
typedef void (*bond_tune_fn)(void *context, unsigned receiver_slot,
                             const bond_announcement *expected_partner);

typedef struct {
    bond_announcement anchor;
    unsigned anchor_slot;
    int requested, pair_verified;
    bond_tune_fn tune;
    void *context;
} bond_discovery_receiver;

/* Init also resets discovery after an explicit restart/configuration change. */
int bond_discovery_receiver_init(bond_discovery_receiver *receiver,
                                 bond_tune_fn tune, void *context);
/* slot is the physical receiver 0 or 1, independent of advertised lane.
 * 1 = first announcement, partner tuning requested once; 2 = reciprocal pair
 * verified; 0 = repeated anchor; -1 = invalid or conflicting announcement.
 * Configuration changes require explicit reset; they never cause retune loops.
 * Callbacks are synchronous; do not re-enter this object. */
int bond_discovery_receiver_receive(bond_discovery_receiver *receiver,
                                    unsigned slot, const uint8_t *wire, size_t length);
/* Call from the per-tuner PMT parser, before the bonding merger. Same return
 * values as receive(); malformed/unrelated descriptors never request tuning. */
int bond_discovery_receiver_descriptor(bond_discovery_receiver *receiver,
                                       unsigned slot, const uint8_t *descriptor,
                                       size_t length);

#ifdef __cplusplus
}
#endif

#endif
