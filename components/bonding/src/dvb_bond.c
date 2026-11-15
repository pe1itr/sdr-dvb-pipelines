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

#include "dvb_bond.h"
#include "bond_discovery.h"
#include "bond_native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PMT_PID 0x1000u
#define DATA_PID 0x1ffeu
#define MAX_DEPTH 8u
#define EVENT_COUNT 8u
#define TRANSPORT_VERSION 3u
#define PES_CAP (2u * BOND_FRAME_SIZE + 9u)

typedef struct { uint64_t due, fraction, last; uint32_t rate; int started; } pace;
typedef struct { unsigned lane; uint8_t frame[BOND_FRAME_SIZE]; } held_frame;
typedef struct {
    uint8_t (*queue)[BOND_FRAME_SIZE];
    unsigned head, count, cc_data, cc_pat, cc_pmt, psi_phase, eos;
    uint64_t ticks, next_psi;
    uint8_t pes[PES_CAP]; size_t length, position;
    pace clock;
} tx_lane;
struct dvb_bond_tx {
    bond_native *native;
    dvb_bond_config_v1 cfg;
    dvb_bond_status_v1 status;
    bond_splitter *splitter;
    uint64_t transport_session;
    tx_lane lane[2];
    unsigned group, groups, lengths[MAX_DEPTH], priority[BOND_MAX_GROUP], source_lane[BOND_MAX_GROUP];
    held_frame held[MAX_DEPTH][2 * (BOND_MAX_GROUP + 2)];
    uint8_t source[BOND_MAX_GROUP][BOND_FRAME_SIZE];
    uint8_t kind[8192], weight[8192]; uint32_t scan[8192];
    uint64_t input_count;
    uint8_t input_bytes[188]; size_t input_size;
    int finished;
};
typedef struct { uint8_t bytes[1024]; size_t count, need; } section;
typedef struct {
    section pat, pmt;
    uint16_t pmt_pid, data_pid;
    uint8_t pes[PES_CAP], previous[3][188];
    size_t count, need;
    unsigned cc[3], logical_lane; int seen[3], discovered;
    uint8_t input_bytes[188]; size_t input_size;
} rx_lane;
struct dvb_bond_rx {
    bond_native *native;
    dvb_bond_config_v1 cfg;
    dvb_bond_status_v1 status;
    bond_merger *m8, *m16, *active;
    uint64_t transport_session, generation;
    unsigned transport_version;
    bond_discovery_receiver discovery;
    rx_lane lane[2];
    dvb_bond_event_v1 events[EVENT_COUNT];
    unsigned event_head, event_count;
    uint64_t event_sequence;
    struct { uint64_t session, transport; } retired[16];
    unsigned retired_next;
    pace clock;
};

static void put(uint8_t *p, uint64_t v, unsigned n) { while (n) { p[--n] = (uint8_t)v; v >>= 8; } }
static uint64_t get(const uint8_t *p, unsigned n) { uint64_t v = 0; while (n--) v = (v << 8) | *p++; return v; }
static uint32_t crc(const uint8_t *p, size_t n, int mpeg)
{
    uint32_t v = UINT32_MAX;
    while (n--) {
        if (mpeg) {
            v ^= (uint32_t)*p++ << 24;
            for (unsigned i = 0; i < 8; ++i) v = (v << 1) ^ (0x04c11db7u & (0u - (v >> 31)));
        } else {
            v ^= *p++;
            for (unsigned i = 0; i < 8; ++i) v = (v >> 1) ^ (0xedb88320u & (0u - (v & 1)));
        }
    }
    return mpeg ? v : ~v;
}
static unsigned group_for(unsigned profile) { return profile == DVB_BOND_PROFILE_XOR8 ? 8 : 16; }
static int shape(const dvb_bond_config_v1 *c)
{
    if (!c || c->struct_size != sizeof(*c) || c->api_version != DVB_BOND_API_V1 ||
        (c->distribution != DVB_BOND_DISTRIBUTION_SHARED &&
         c->distribution != DVB_BOND_DISTRIBUTION_SEPARATE_LANES) ||
        (c->distribution == DVB_BOND_DISTRIBUTION_SHARED && (c->profile < 1 || c->profile > 3)) || c->interleave_groups < 1 || c->interleave_groups > MAX_DEPTH ||
        c->lane_bitrate != 30160 || c->announcement_ms < 100 || c->announcement_ms > 60000 ||
        c->tx_queue_frames < 8 || c->tx_queue_frames > 1024 ||
        c->rx_window_groups < 1 || c->rx_window_groups > BOND_MAX_WINDOW ||
        c->playout_delay_ns > UINT64_C(60000000000)) return 0;
    for (unsigned i = 0; i < 7; ++i) if (c->reserved[i]) return 0;
    return 1;
}
void dvb_bond_config_init_v1(dvb_bond_config_v1 *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c)); c->struct_size = sizeof(*c); c->api_version = 1;
    c->bond_id = c->session = 1; c->frequency_hz[0] = 144600000; c->frequency_hz[1] = 144650000;
    c->source_bitrate = 50100; c->lane_bitrate = 30160; c->profile = 2; c->interleave_groups = 1;
    c->announcement_ms = 2000; c->tx_queue_frames = 64; c->rx_window_groups = 64;
    c->revision = 1; c->playout_delay_ns = UINT64_C(8000000000);
}
uint32_t dvb_bond_api_version(void) { return DVB_BOND_API_V1; }
static uint64_t announcement_ticks(const dvb_bond_config_v1 *c)
{ return (uint64_t)c->announcement_ms * c->lane_bitrate / 1504000; }
static uint64_t control_bitrate(const dvb_bond_config_v1 *c)
{
    uint64_t ticks = announcement_ticks(c);
    return ticks ? (2 * (uint64_t)c->lane_bitrate + ticks - 1) / ticks : c->lane_bitrate;
}
uint32_t dvb_bond_source_limit_v1(const dvb_bond_config_v1 *c)
{
    if (!shape(c)) return 0;
    if (c->distribution == DVB_BOND_DISTRIBUTION_SEPARATE_LANES) return 2 * c->lane_bitrate;
    uint64_t control = control_bitrate(c);
    if (control >= c->lane_bitrate) return 0;
    unsigned k = group_for(c->profile), r = c->profile == 3 ? 2 : 1;
    return (uint32_t)(2 * (c->lane_bitrate - control) * k * 14 / (15 * (k + r)));
}
int dvb_bond_tx_input_contract_json_v1(const dvb_bond_config_v1 *c, char *buffer, size_t capacity)
{
    if (!buffer || !capacity || !shape(c) || c->source_bitrate < 15040 ||
        c->source_bitrate > dvb_bond_source_limit_v1(c)) return -1;
    int separate = c->distribution == DVB_BOND_DISTRIBUTION_SEPARATE_LANES;
    /* Public delivery requirements, not a description of the transport.
     * Limits count ordinary TS/PES data; the generator need not implement or
     * identify channels, protection, discovery, or reconstruction. */
    const char *limits = separate ?
        "{\"video_and_system_ts_max_bps\":24128,\"audio_and_si_ts_max_bps\":24128,"
        "\"include_si_in_each_budget\":true,\"audio_budget_payload_bytes_per_ts_packet\":176,"
        "\"audio_budget_rounding\":\"per_pes\",\"pat_max_section_bytes\":183,\"pmt_max_section_bytes\":121}" : "null";
    int n = snprintf(buffer, capacity,
        "{\"schema_version\":1,\"mode\":\"%s\","
        "\"input\":{\"format\":\"mpegts\",\"packet_size_bytes\":188,\"pacing\":\"cbr\",\"total_ts_bitrate_bps\":%u,"
        "\"single_program_required\":%s,\"clear_ts_required\":%s,\"audio_stream_count\":%s,"
        "\"pat_pmt_before_media_required\":%s,\"pcr_pid_requirement\":\"%s\","
        "\"pcr_before_audio_required\":%s,\"common_clock_required\":true},"
        "\"timing\":{\"pcr_max_gap_ms\":100,\"pat_max_gap_ms\":500,\"pmt_max_gap_ms\":500},"
        "\"bitrate_limits\":%s}",
        separate ? "DVB-T bonding lanes" : "DVB-T bonding shared", c->source_bitrate,
        separate ? "true" : "false", separate ? "true" : "false", separate ? "1" : "null",
        separate ? "true" : "false", separate ? "video" : "program_clock", separate ? "true" : "false", limits);
    return n < 0 || (size_t)n >= capacity ? -1 : n;
}
static uint64_t transport_session(const bond_announcement *a, const dvb_bond_config_v1 *c, unsigned version)
{
    uint64_t words[] = {a->bond_id, a->session, a->revision, c->distribution,
        c->profile, c->interleave_groups, a->source_bitrate, c->lane_bitrate,
        a->interval_ms, a->bandwidth_hz, a->rf_profile,
        a->lane ? a->partner_frequency_hz : a->own_frequency_hz,
        a->lane ? a->own_frequency_hz : a->partner_frequency_hz};
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        for (unsigned j = 0; j < 8; ++j) {
            hash ^= (words[i] >> (56 - 8 * j)) & 255;
            hash *= UINT64_C(1099511628211);
        }
    if (version >= 3) for (unsigned j = 0; j < 8; ++j) {
        hash ^= ((uint64_t)version >> (56 - 8 * j)) & 255;
        hash *= UINT64_C(1099511628211);
    }
    return hash ? hash : 1;
}
static void status_init(dvb_bond_status_v1 *s, const dvb_bond_config_v1 *c)
{
    memset(s, 0, sizeof(*s)); s->struct_size = sizeof(*s); s->api_version = 1;
    s->source_bitrate = c->source_bitrate; s->lane_bitrate = c->lane_bitrate;
    s->profile = c->distribution == DVB_BOND_DISTRIBUTION_SHARED ? c->profile : DVB_BOND_PROFILE_NONE;
    s->interleave_groups = c->distribution == DVB_BOND_DISTRIBUTION_SHARED ? c->interleave_groups : 1;
    s->distribution = c->distribution;
}
static int clock_ready(pace *p, uint64_t now)
{
    if (p->started && now < p->last) return -1;
    p->last = now;
    if (!p->started) { p->started = 1; p->due = now; }
    return now >= p->due;
}
static int clock_step(pace *p)
{
    uint64_t q = UINT64_C(1504000000000) / p->rate;
    p->fraction += UINT64_C(1504000000000) % p->rate;
    if (p->fraction >= p->rate) { ++q; p->fraction -= p->rate; }
    if (p->due > UINT64_MAX - q) return -1;
    p->due += q; return 0;
}
static bond_announcement announcement(const dvb_bond_tx *t, unsigned lane)
{
    bond_announcement a = {0};
    a.bond_id = t->cfg.bond_id; a.session = t->cfg.session; a.revision = t->cfg.revision;
    a.lane = lane; a.group_size = t->group; a.own_frequency_hz = t->cfg.frequency_hz[lane];
    a.partner_frequency_hz = t->cfg.frequency_hz[1 - lane]; a.bandwidth_hz = 40000;
    a.source_bitrate = t->cfg.source_bitrate; a.interval_ms = t->cfg.announcement_ms;
    a.rf_profile = BOND_RF_DVBT_NARROW_QPSK_2K_1_2_GI_1_32; return a;
}
static void enqueue(dvb_bond_tx *t, unsigned lane, const uint8_t *frame)
{
    tx_lane *l = &t->lane[lane];
    if (l->count == t->cfg.tx_queue_frames) { ++t->status.queue_drops[lane]; return; }
    memcpy(l->queue[(l->head + l->count++) % t->cfg.tx_queue_frames], frame, BOND_FRAME_SIZE);
}
static void hold(dvb_bond_tx *t, unsigned lane, const uint8_t *frame)
{
    held_frame *h = &t->held[t->groups][t->lengths[t->groups]++];
    h->lane = lane; memcpy(h->frame, frame, BOND_FRAME_SIZE);
}
static void release(dvb_bond_tx *t)
{
    for (unsigned i = 0; i < 2 * (BOND_MAX_GROUP + 2); ++i)
        for (unsigned g = 0; g < t->groups; ++g)
            if (i < t->lengths[g]) enqueue(t, t->held[g][i].lane, t->held[g][i].frame);
    memset(t->lengths, 0, sizeof(t->lengths)); t->groups = 0;
}
static void emitted(void *context, unsigned lane, const uint8_t *f)
{
    dvb_bond_tx *t = context; unsigned index = f[6], count = f[5];
    if (index < count) {
        memcpy(t->source[index], f, BOND_FRAME_SIZE); t->source_lane[index] = lane; hold(t, lane, f);
    } else {
        if (t->cfg.profile == DVB_BOND_PROFILE_HYBRID16) {
            unsigned best = 0;
            for (unsigned i = 1; i < count; ++i)
                if (t->priority[i] > t->priority[best]) best = i;
            hold(t, t->source_lane[best], f);
            hold(t, 1 - t->source_lane[best], t->source[best]);
        } else hold(t, lane, f);
        memset(t->priority, 0, sizeof(t->priority));
        if (++t->groups == t->cfg.interleave_groups) release(t);
    }
}
dvb_bond_tx *dvb_bond_tx_create_v1(const dvb_bond_config_v1 *c)
{
    if (!shape(c) || !c->session || !c->bond_id || !c->source_bitrate ||
        (c->distribution == DVB_BOND_DISTRIBUTION_SEPARATE_LANES && c->source_bitrate < 15040) ||
        c->source_bitrate > dvb_bond_source_limit_v1(c) ||
        !c->frequency_hz[0] || !c->frequency_hz[1] || c->frequency_hz[0] > INT64_MAX || c->frequency_hz[1] > INT64_MAX ||
        (c->frequency_hz[1] > c->frequency_hz[0] ? c->frequency_hz[1] - c->frequency_hz[0] :
         c->frequency_hz[0] - c->frequency_hz[1]) != 50000 ||
        (c->distribution != DVB_BOND_DISTRIBUTION_SEPARATE_LANES && c->tx_queue_frames < ((group_for(c->profile) + (c->profile == 3 ? 2u : 1u)) *
                             c->interleave_groups + 1u) / 2u)) return NULL;
    dvb_bond_tx *t = calloc(1, sizeof(*t)); if (!t) return NULL;
    t->cfg = *c; t->group = group_for(c->profile); status_init(&t->status, c);
    t->status.session = c->session; t->status.bond_id = c->bond_id;
    memcpy(t->status.frequency_hz, c->frequency_hz, sizeof(c->frequency_hz));
    if (c->distribution == DVB_BOND_DISTRIBUTION_SEPARATE_LANES) {
        t->native = bond_native_create(c);
        if (!t->native) { free(t); return NULL; }
        return t;
    }
    bond_announcement a = announcement(t, 0);
    t->transport_session = transport_session(&a, c, TRANSPORT_VERSION);
    t->splitter = bond_splitter_create(t->transport_session, t->group, emitted, t);
    for (unsigned i = 0; i < 2; ++i) {
        t->lane[i].queue = calloc(c->tx_queue_frames, BOND_FRAME_SIZE);
        t->lane[i].clock.rate = c->lane_bitrate;
    }
    if (!t->splitter || !t->lane[0].queue || !t->lane[1].queue) { dvb_bond_tx_destroy(t); return NULL; }
    return t;
}
void dvb_bond_tx_destroy(dvb_bond_tx *t)
{
    if (t) { bond_native_destroy(t->native); bond_splitter_destroy(t->splitter); free(t->lane[0].queue); free(t->lane[1].queue); free(t); }
}
static unsigned weight(dvb_bond_tx *t, const uint8_t *p)
{
    unsigned pid = ((p[1] & 31) << 8) | p[2], a = (p[3] >> 4) & 3, w = 0;
    if (pid == 8191) return 0;
    if ((a == 2 || a == 3) && p[4] && p[4] <= 183 && (p[5] & 0x10)) w = 30;
    if (a != 1 && a != 3) return w;
    unsigned off = a == 3 ? 5u + p[4] : 4;
    if (off >= 188) return w;
    if (p[1] & 0x40) {
        t->kind[pid] = t->weight[pid] = 0; t->scan[pid] = 0xffffffffu;
        if (off + 9 <= 188 && !memcmp(p + off, "\0\0\1", 3)) {
            unsigned id = p[off + 3];
            if (id >= 0xe0 && id <= 0xef) t->kind[pid] = 1;
            if (id >= 0xc0 && id <= 0xdf) t->weight[pid] = 10;
            off += 9u + p[off + 8];
        } else w = 30;
    }
    if (t->kind[pid] == 1) for (; off < 188; ++off) {
        t->scan[pid] = (t->scan[pid] << 8) | p[off];
        if ((t->scan[pid] & 0xffffff00u) == 0x00000100u) {
            unsigned type = t->scan[pid] & 31;
            if (type == 5) t->weight[pid] = 100;
            else if ((type == 7 || type == 8) && t->weight[pid] < 80) t->weight[pid] = 80;
        }
    }
    return w > t->weight[pid] ? w : t->weight[pid];
}
int dvb_bond_tx_push(dvb_bond_tx *t, const uint8_t p[188])
{
    if (!t || !p || t->input_size || t->finished || p[0] != 0x47 || t->input_count == UINT64_MAX) return -1;
    if (t->native) { int rc = bond_native_tx_push(t->native, p); if (!rc) ++t->input_count; return rc; }
    unsigned index = (unsigned)((t->input_count / 7) % t->group);
    t->priority[index] += weight(t, p);
    if (bond_splitter_push(t->splitter, p)) return -1;
    ++t->input_count; ++t->status.input_packets[0]; return 0;
}
int dvb_bond_tx_write(dvb_bond_tx *t, const uint8_t *p, size_t n)
{
    if (!t || (!p && n) || t->finished) return -1;
    while (n) {
        size_t part = 188 - t->input_size; if (part > n) part = n;
        memcpy(t->input_bytes + t->input_size, p, part); t->input_size += part; p += part; n -= part;
        if (t->input_size == 188) { t->input_size = 0; if (dvb_bond_tx_push(t, t->input_bytes)) return -1; }
    }
    return 0;
}
int dvb_bond_tx_finish(dvb_bond_tx *t)
{
    if (!t || t->input_size || t->finished) return -1;
    if (t->native) { t->finished = 1; return bond_native_finish(t->native); }
    if (bond_splitter_finish(t->splitter)) return -1;

    release(t); t->finished = 1; t->status.total_source_packets = t->input_count; return 0;
}
static void psi_packet(uint8_t *p, unsigned pid, unsigned *cc, const uint8_t *s, size_t n)
{
    memset(p, 0xff, 188); p[0] = 0x47; p[1] = 0x40 | (uint8_t)(pid >> 8); p[2] = (uint8_t)pid;
    p[3] = 0x10 | (uint8_t)((*cc)++ & 15); p[4] = 0; memcpy(p + 5, s, n);
}
static void pat(dvb_bond_tx *t, unsigned lane, uint8_t *p)
{
    uint8_t s[16] = {0,0xb0,13,0,1,0xc1,0,0,0,1,0xf0,0,0,0,0,0};
    s[5] |= (uint8_t)((t->cfg.revision & 31) << 1); put(s + 12, crc(s, 12, 1), 4);
    psi_packet(p, 0, &t->lane[lane].cc_pat, s, sizeof(s));
}
static void pmt(dvb_bond_tx *t, unsigned lane, uint8_t *p)
{
    uint8_t s[128] = {2,0xb0,0,0,1,0xc1,0,0,0xff,0xff,0xf0,80};
    s[5] |= (uint8_t)((t->cfg.revision & 31) << 1);
    bond_announcement a = announcement(t, lane);
    (void)bond_announcement_descriptor_encode(&a, s + 12);
    uint8_t *x = s + 78; x[0] = 0x81; x[1] = 12; memcpy(x + 2, "DBNP", 4);
    x[6] = TRANSPORT_VERSION; x[7] = (uint8_t)t->cfg.profile; x[8] = (uint8_t)t->cfg.interleave_groups; x[9] = (uint8_t)t->cfg.distribution;
    put(x + 10, t->cfg.lane_bitrate, 4);
    size_t n = 92; s[n++] = 6; s[n++] = 0xff; s[n++] = 0xfe; s[n++] = 0xf0; s[n++] = 0;
    put(s + 1, 0xb000u | (n + 4 - 3), 2); put(s + n, crc(s, n, 1), 4); n += 4;
    psi_packet(p, PMT_PID, &t->lane[lane].cc_pmt, s, n);
}
static void start_pes(tx_lane *l, const uint8_t *record, size_t n)
{
    uint8_t header[9] = {0,0,1,0xbd,0,0,0x80,0,0};
    put(header + 4, n + 3, 2); memcpy(l->pes, header, 9); memcpy(l->pes + 9, record, n);
    l->length = n + 9; l->position = 0;
}
int dvb_bond_tx_pop(dvb_bond_tx *t, unsigned lane, uint64_t now, uint8_t p[188])
{
    if (!t || lane > 1 || !p) return -1;
    if (t->native) return bond_native_tx_pop(t->native, lane, now, p);
    tx_lane *l = &t->lane[lane];
    if (t->finished && !l->count && l->position == l->length && l->eos == 2) return DVB_BOND_END;
    int ready = clock_ready(&l->clock, now); if (ready <= 0) return ready;
    if (!l->psi_phase && l->ticks >= l->next_psi) l->psi_phase = 1;
    if (l->psi_phase == 1) {
        pat(t, lane, p); l->psi_phase = 2;
        l->next_psi = l->ticks + announcement_ticks(&t->cfg);
    } else if (l->psi_phase == 2) { pmt(t, lane, p); l->psi_phase = 0; }
    else {
        unsigned batch = 2;
        if (l->position == l->length && (l->count >= batch || (t->finished && l->count))) {
            unsigned records = l->count >= batch ? batch : l->count;
            uint8_t header[9] = {0,0,1,0xbd,0,0,0x80,0,0};
            size_t length = 9;
            for (unsigned i = 0; i < records; ++i) {
                memcpy(l->pes + length, l->queue[l->head], BOND_FRAME_SIZE);
                length += BOND_FRAME_SIZE;
                l->head = (l->head + 1) % t->cfg.tx_queue_frames; --l->count;
            }
            put(header + 4, length - 6, 2); memcpy(l->pes, header, 9);
            l->length = length; l->position = 0;
        } else if (l->position == l->length && t->finished && l->eos < 2) {
            uint8_t end[28] = {'D','B','N','E',1,0,0,0};
            put(end + 8, t->transport_session, 8); put(end + 16, t->input_count, 8);
            put(end + 24, crc(end, 24, 0), 4); start_pes(l, end, sizeof(end)); ++l->eos;
        }
        if (l->position < l->length) {
            size_t n = l->length - l->position; if (n > 184) n = 184;
            memset(p, 0xff, 188); p[0] = 0x47; p[1] = (uint8_t)(DATA_PID >> 8);
            if (!l->position) p[1] |= 0x40;
            p[2] = (uint8_t)DATA_PID; p[3] = 0x10 | (uint8_t)(l->cc_data++ & 15);
            size_t off = 4;
            if (n < 184) { p[3] |= 0x20; p[4] = (uint8_t)(183 - n); if (p[4]) p[5] = 0; off = 188 - n; }
            memcpy(p + off, l->pes + l->position, n); l->position += n;
        } else { memset(p, 0xff, 188); p[0] = 0x47; p[1] = 0x1f; p[2] = 0xff; p[3] = 0x10; }
    }
    ++l->ticks; ++t->status.output_packets[lane];
    return clock_step(&l->clock) ? -1 : DVB_BOND_PACKET;
}
uint64_t dvb_bond_tx_next_due_ns(const dvb_bond_tx *t, unsigned lane)
{ return t && lane < 2 ? (t->native ? bond_native_due(t->native, lane) : t->lane[lane].clock.due) : UINT64_MAX; }
int dvb_bond_tx_status(const dvb_bond_tx *t, dvb_bond_status_v1 *s)
{ if (!t || !s) return -1; if (t->native) bond_native_status(t->native, s); else *s = t->status; s->ended = t->finished; return 0; }

static void event_push(dvb_bond_rx *r, dvb_bond_event_v1 *e)
{
    e->struct_size = sizeof(*e); e->api_version = 1; e->sequence = ++r->event_sequence;
    if (r->event_count == EVENT_COUNT) { ++r->status.event_overflows; return; }
    r->events[(r->event_head + r->event_count++) % EVENT_COUNT] = *e;
}
static void tune(void *context, unsigned slot, const bond_announcement *a)
{
    dvb_bond_rx *r = context; dvb_bond_event_v1 e = {0};
    e.type = 1; e.receiver_slot = slot; e.lane = a->lane; e.rf_profile = a->rf_profile;
    e.bond_id = a->bond_id; e.session = a->session; e.frequency_hz = a->own_frequency_hz;
    e.anchor_frequency_hz = a->partner_frequency_hz; e.partner_offset_hz = -bond_announcement_offset_hz(a);
    e.bandwidth_hz = 40000; e.source_bitrate = a->source_bitrate;
    e.lane_bitrate = r->status.lane_bitrate; e.profile = r->status.profile;
    e.distribution = r->status.distribution; e.interleave_groups = r->status.interleave_groups; e.guard_denominator = 32;
    e.fec_numerator = 1; e.fec_denominator = 2; event_push(r, &e);
    r->status.tune_request_sequence = e.sequence; r->status.tune_frequency_hz = e.frequency_hz;
    r->status.tune_offset_hz = e.partner_offset_hz; r->status.tune_receiver_slot = slot; r->status.tune_lane = a->lane;
}
int dvb_bond_rx_reset(dvb_bond_rx *r)
{
    if (!r) return -1;
    dvb_bond_config_v1 c = r->cfg;
    bond_native *native = r->native; if (native) bond_native_reset(native);
    bond_merger *a = r->m8, *b = r->m16; uint64_t sequence = r->event_sequence;
    memset(r, 0, sizeof(*r)); r->cfg = c; r->native = native;
    r->m8 = a; r->m16 = b; r->event_sequence = sequence;
    status_init(&r->status, &c);
    (void)bond_merger_reset(a, 0, 0); (void)bond_merger_reset(b, 0, 0);
    for (unsigned i = 0; i < 2; ++i) {
        r->lane[i].pmt_pid = PMT_PID; r->lane[i].data_pid = DATA_PID;
    }
    return bond_discovery_receiver_init(&r->discovery, tune, r);
}
dvb_bond_rx *dvb_bond_rx_create_v1(const dvb_bond_config_v1 *c)
{
    if (!shape(c)) return NULL;
    dvb_bond_rx *r = calloc(1, sizeof(*r)); if (!r) return NULL;
    r->cfg = *c;
    r->native = bond_native_create(c); if (!r->native) { free(r); return NULL; }
    r->m8 = bond_merger_create(0, 8, c->rx_window_groups);
    r->m16 = bond_merger_create(0, 16, c->rx_window_groups);
    if (!r->m8 || !r->m16) { dvb_bond_rx_destroy(r); return NULL; }
    (void)dvb_bond_rx_reset(r); return r;
}
void dvb_bond_rx_destroy(dvb_bond_rx *r)
{
    if (r) {
        bond_native_destroy(r->native);
        bond_merger_destroy(r->m8); bond_merger_destroy(r->m16);
        free(r);
    }
}
static void section_done(dvb_bond_rx *r, unsigned slot, section *s)
{
    const uint8_t *p = s->bytes; size_t n = s->need;
    if (n < 12 || crc(p, n, 1) || !(p[5] & 1)) { ++r->status.malformed; return; }
    if (p[0] == 0) {
        for (size_t i = 8; i + 4 <= n - 4; i += 4)
            if (get(p + i, 2)) { r->lane[slot].pmt_pid = (uint16_t)(get(p + i + 2, 2) & 8191); break; }
        return;
    }
    if (p[0] != 2 || n < 16) return;
    size_t end = 12 + (get(p + 10, 2) & 4095);
    if (end > n - 4) { ++r->status.malformed; return; }
    bond_announcement a = {0}; int have = 0; unsigned profile = 0, depth = 0, bitrate = 0, distribution = 0, version = 0;
    for (size_t i = 12; i + 2 <= end;) {
        size_t length = p[i + 1] + 2u; if (i + length > end) { ++r->status.malformed; return; }
        if (p[i] == 0x80 && !bond_announcement_descriptor_decode(&a, p + i, length)) have = 1;
        if (p[i] == 0x81 && length == 14 && !memcmp(p + i + 2, "DBNP", 4) &&
            (p[i + 6] >= 1 && p[i + 6] <= 3 && !p[i + 9])) {
            version = p[i + 6]; profile = p[i + 7]; depth = p[i + 8];
            distribution = p[i + 9]; bitrate = (unsigned)get(p + i + 10, 4);
        }
        i += length;
    }
    uint16_t data_pid = 0;
    for (size_t i = end; i + 5 <= n - 4;) {
        size_t length = 5 + (get(p + i + 3, 2) & 4095);
        if (i + length > n - 4) { ++r->status.malformed; return; }
        if (p[i] == 6) data_pid = (uint16_t)(get(p + i + 1, 2) & 8191);
        i += length;
    }
    dvb_bond_config_v1 c = r->cfg; c.profile = profile; c.interleave_groups = depth;
    c.lane_bitrate = bitrate; c.distribution = distribution;
    uint64_t delta = a.own_frequency_hz > a.partner_frequency_hz ?
        a.own_frequency_hz - a.partner_frequency_hz : a.partner_frequency_hz - a.own_frequency_hz;
    if (!have || !a.bond_id || !data_pid || !shape(&c) || a.group_size != group_for(profile) ||
        a.bandwidth_hz != 40000 || delta != 50000 || !a.session ||
        (r->cfg.session && r->cfg.session != a.session) || a.source_bitrate > dvb_bond_source_limit_v1(&c)) {
        ++r->status.malformed; return;
    }
    if (r->discovery.requested && a.session == r->status.session && version < r->transport_version) {
        ++r->status.malformed; return;
    }
    uint64_t wire_session = version == 1 ? a.session : transport_session(&a, &c, version);
    uint64_t generation = transport_session(&a, &c, version) ^ (version == 1 ? UINT64_C(0x8a93e7b5) : 0);
    for (unsigned i = 0; i < 16; ++i)
        if (a.session == r->retired[i].session &&
            (!r->retired[i].transport || generation == r->retired[i].transport)) {
            ++r->status.malformed; return;
        }
    if (bond_native_active(r->native)) {
        bond_native_deactivate(r->native);
        dvb_bond_event_v1 e = {0}; e.type = DVB_BOND_EVENT_RESTART; e.session = a.session;
        e.distribution = distribution; e.profile = profile; event_push(r, &e);
    }
    if (r->discovery.requested && generation != r->generation) {
        if (a.bond_id != r->status.bond_id || a.own_frequency_hz != r->status.frequency_hz[a.lane] ||
            a.partner_frequency_hz != r->status.frequency_hz[1 - a.lane]) { ++r->status.malformed; return; }
        /* Legacy records have no configuration discriminator within a session.
         * A legacy TX must restart with a new session to avoid mixing old data. */
        if (version == 1 && a.session == r->status.session) { ++r->status.malformed; return; }
        uint64_t sessions[16], transports[16];
        for (unsigned i = 0; i < 16; ++i) {
            sessions[i] = r->retired[i].session; transports[i] = r->retired[i].transport;
        }
        unsigned next = r->retired_next;
        sessions[next % 16] = r->status.session;
        transports[next++ % 16] = a.session == r->status.session ? r->generation : 0;
        bond_native *native = r->native; r->native = NULL;
        (void)dvb_bond_rx_reset(r); r->native = native;
        for (unsigned i = 0; i < 16; ++i) {
            r->retired[i].session = sessions[i]; r->retired[i].transport = transports[i];
        }
        r->retired_next = next;
        dvb_bond_event_v1 e = {0}; e.type = DVB_BOND_EVENT_RESTART;
        e.bond_id = a.bond_id; e.session = a.session; e.profile = profile;
        e.distribution = distribution; e.source_bitrate = a.source_bitrate;
        e.lane_bitrate = bitrate; e.interleave_groups = depth; event_push(r, &e);
    }
    /* Validate discovery against a copy before publishing metadata or events. */
    uint8_t wire[BOND_ANNOUNCEMENT_SIZE]; (void)bond_announcement_encode(&a, wire);
    bond_discovery_receiver probe = r->discovery;
    /* An existing generation must have reciprocal RF/source metadata. */
    if (probe.requested && bond_discovery_receiver_receive(&probe, slot, wire, sizeof(wire)) < 0) {
        ++r->status.malformed; return;
    }
    r->status.profile = profile; r->status.interleave_groups = depth;
    r->status.distribution = distribution; r->status.lane_bitrate = bitrate;
    r->transport_session = wire_session; r->generation = generation; r->transport_version = version;
    int result = bond_discovery_receiver_receive(&r->discovery, slot, wire, sizeof(wire));
    if (result < 0) { ++r->status.malformed; return; }
    r->lane[slot].data_pid = data_pid; r->lane[slot].logical_lane = a.lane; r->lane[slot].discovered = 1; r->status.discovered_mask |= 1u << a.lane;
    r->status.session = a.session; r->status.bond_id = a.bond_id; r->status.source_bitrate = a.source_bitrate;
    r->status.frequency_hz[a.lane] = a.own_frequency_hz; r->status.frequency_hz[1 - a.lane] = a.partner_frequency_hz;
    if (result == 2 && !r->status.pair_verified) {
        r->status.pair_verified = 1; dvb_bond_event_v1 e = {0}; e.type = 2; e.bond_id = a.bond_id;
        e.session = a.session; e.distribution = distribution; e.profile = profile; event_push(r, &e);
    }
}
static void section_feed(dvb_bond_rx *r, unsigned slot, section *s, const uint8_t *p, size_t n, int start)
{
    if (start) {
        if (!n || (size_t)p[0] + 1 > n) { s->count = s->need = 0; return; }
        /* Our PAT/PMT fit one packet; continuation is supported for sections
         * begun here. A new PUSI discards a previously incomplete section. */
        size_t pointer = p[0] + 1u; p += pointer; n -= pointer; s->count = s->need = 0;
    } else if (!s->count) return;
    while (n && s->count < sizeof(s->bytes)) {
        if (!s->count && *p == 0xff) return;
        s->bytes[s->count++] = *p++; --n;
        if (s->count == 3) {
            s->need = 3 + (get(s->bytes + 1, 2) & 4095);
            if (s->need < 12 || s->need > sizeof(s->bytes)) { s->count = s->need = 0; ++r->status.malformed; return; }
        }
        if (s->need && s->count == s->need) { section_done(r, slot, s); s->count = s->need = 0; }
    }
}
static int record(dvb_bond_rx *r, unsigned slot, const uint8_t *p, size_t n, uint64_t now)
{
    if (n == 28 && !memcmp(p, "DBNE", 4) && p[4] == 1 && !p[5] && !p[6] && !p[7] &&
        get(p + 24, 4) == crc(p, 24, 0)) {
        if (!r->discovery.requested || get(p + 8, 8) != r->transport_session) return 1;
        uint64_t total = get(p + 16, 8);
        if (r->status.ended && total != r->status.total_source_packets) { ++r->status.malformed; return 1; }
        if (!r->status.ended) { dvb_bond_event_v1 e = {0}; e.type = 3; e.session = r->status.session; event_push(r, &e); }
        r->status.ended = 1; r->status.total_source_packets = total; return 1;
    }
    bond_frame_info info;
    if (bond_frame_inspect(p, n, &info)) return 0;
    if (!r->discovery.requested) return 1;
    if (info.session != r->transport_session || info.source_count > group_for(r->status.profile)) {
        ++r->status.malformed; return 1;
    }
    if (!r->lane[slot].discovered) return 1;
    if (!r->active) {
        if (now > UINT64_MAX - r->cfg.playout_delay_ns) { ++r->status.malformed; return 1; }
        bond_merger *m = r->status.profile == 1 ? r->m8 : r->m16;
        if (bond_merger_reset(m, info.session, info.group_base)) { ++r->status.malformed; return 1; }
        r->active = m;
        r->status.active = 1; r->status.next_source_packet = info.group_base * 7;
        r->clock.started = 1; r->clock.due = now + r->cfg.playout_delay_ns; r->clock.last = now;
        r->clock.rate = r->status.source_bitrate;
    }
    if (bond_merger_receive(r->active, p, n)) ++r->status.malformed;
    return 1;
}
/* DBND/DBNE have independent CRCs. Scan across a CC gap so that a damaged
 * first record in a packed PES does not throw away the intact second record.
 * On a false magic or CRC failure, advance one byte and resynchronize. */
static void record_bytes(dvb_bond_rx *r, unsigned slot, rx_lane *l, const uint8_t *p, size_t n, uint64_t now)
{
    while (n--) {
        l->pes[l->count++] = *p++;
        for (;;) {
            if (l->count < 4) break;
            size_t need = !memcmp(l->pes, "DBND", 4) ? BOND_FRAME_SIZE :
                          !memcmp(l->pes, "DBNE", 4) ? 28 : 0;

            if (need && l->count < need) break;
            if (need && record(r, slot, l->pes, need, now)) { l->count = 0; break; }
            if (need) ++r->status.malformed;
            memmove(l->pes, l->pes + 1, --l->count);
        }
    }
}
int dvb_bond_rx_push(dvb_bond_rx *r, unsigned slot, const uint8_t p[188], uint64_t now)
{
    if (!r || !p || slot > 1 || r->lane[slot].input_size || p[0] != 0x47) return -1;
    int was_native = bond_native_active(r->native);
    if (bond_native_rx_push(r->native, slot, p, now)) return -1;
    if (bond_native_active(r->native) && !was_native) {
        bond_native *native = r->native;
        uint64_t previous_session = r->discovery.requested ? r->status.session : 0;
        uint8_t retired[sizeof(r->retired)]; memcpy(retired, r->retired, sizeof(retired));
        unsigned retired_next = r->retired_next;
        r->native = NULL; (void)dvb_bond_rx_reset(r); r->native = native;
        memcpy(r->retired, retired, sizeof(retired)); r->retired_next = retired_next;
        bond_native_status(native, &r->status);
        if (previous_session) bond_native_note_restart(native);
        if (previous_session && previous_session != r->status.session) {
            unsigned index = r->retired_next++ % 16;
            r->retired[index].session = previous_session; r->retired[index].transport = 0;
        }
    }
    /* Still parse PSI so a later coded-mode announcement can reset native mode. */
    if (bond_native_active(r->native) && (((p[1] & 31u) << 8) | p[2]) != 0 &&
        (((p[1] & 31u) << 8) | p[2]) != r->lane[slot].pmt_pid) return 0;
    uint8_t local[188]; memcpy(local, p, sizeof(local)); p = local;
    ++r->status.input_packets[slot]; rx_lane *l = &r->lane[slot];
    unsigned pid = ((p[1] & 31) << 8) | p[2], a = (p[3] >> 4) & 3;
    int index = pid == 0 ? 0 : pid == l->pmt_pid ? 1 : pid == l->data_pid ? 2 : -1;
    if (index < 0) return 0;
    if ((p[1] & 0x80) || (p[3] & 0xc0) || !a || ((a == 2 || a == 3) && p[4] > 183)) {
        l->count = l->need = 0; l->pat.count = l->pmt.count = 0; memset(l->seen, 0, sizeof(l->seen)); ++r->status.malformed; return 0;
    }
    if (a == 2 || a == 3) if (p[4] && (p[5] & 0x80)) l->seen[index] = 0;
    if (a == 2) return 0;
    unsigned cc = p[3] & 15; int start = (p[1] & 0x40) != 0;
    if (l->seen[index] && cc == l->cc[index] && !memcmp(l->previous[index], p, 188)) return 0;
    if (l->seen[index] && cc != ((l->cc[index] + 1) & 15)) {
        ++r->status.continuity_errors;
        if (index == 2) l->count = l->need = 0;
        else { section *s = index ? &l->pmt : &l->pat; s->count = s->need = 0; }
    }
    l->seen[index] = 1; l->cc[index] = cc; memcpy(l->previous[index], p, 188);
    size_t off = a == 3 ? 5u + p[4] : 4;
    if (off >= 188) return 0;
    if (index < 2) { section_feed(r, slot, index ? &l->pmt : &l->pat, p + off, 188 - off, start); return 0; }
    if (start) {
        l->count = l->need = 0;
        if (off + 9 <= 188 && !memcmp(p + off, "\0\0\1\xbd", 4) &&
            p[off + 6] == 0x80 && !p[off + 7] && !p[off + 8]) off += 9;
        else ++r->status.malformed;
    }
    record_bytes(r, slot, l, p + off, 188 - off, now);
    return 0;
}
int dvb_bond_rx_finish(dvb_bond_rx *r)
{
    if (!r || !bond_native_active(r->native) || r->lane[0].input_size || r->lane[1].input_size) return -1;
    return bond_native_finish(r->native);
}
int dvb_bond_rx_write(dvb_bond_rx *r, unsigned slot, const uint8_t *p, size_t n, uint64_t now)
{
    if (!r || slot > 1 || (!p && n)) return -1;
    rx_lane *l = &r->lane[slot];
    while (n) {
        size_t part = 188 - l->input_size; if (part > n) part = n;
        memcpy(l->input_bytes + l->input_size, p, part); l->input_size += part; p += part; n -= part;
        if (l->input_size == 188) { l->input_size = 0; if (dvb_bond_rx_push(r, slot, l->input_bytes, now)) return -1; }
    }
    return 0;
}
int dvb_bond_rx_pop(dvb_bond_rx *r, uint64_t now, uint8_t p[188])
{
    if (!r || !p) return -1;
    if (bond_native_active(r->native)) return bond_native_rx_pop(r->native, now, p);
    if (r->status.ended && (!r->active || r->status.next_source_packet >= r->status.total_source_packets)) return 2;
    if (!r->active) return 0;
    int ready = clock_ready(&r->clock, now); if (ready <= 0) return ready;
    int delivered = bond_merger_pop(r->active, p); if (delivered < 0) return -1;
    ++r->status.next_source_packet; ++r->status.output_packets[0];
    return clock_step(&r->clock) ? -1 : 1;
}
uint64_t dvb_bond_rx_next_due_ns(const dvb_bond_rx *r)
{ return r && bond_native_active(r->native) ? bond_native_due(r->native, 0) : r && r->active ? r->clock.due : UINT64_MAX; }
int dvb_bond_rx_poll_event(dvb_bond_rx *r, dvb_bond_event_v1 *e)
{
    if (!r || !e) return -1;
    if (bond_native_active(r->native)) return bond_native_event(r->native, e);
    if (!r->event_count) return 0;
    *e = r->events[r->event_head]; r->event_head = (r->event_head + 1) % EVENT_COUNT; --r->event_count; return 1;
}
int dvb_bond_rx_status(const dvb_bond_rx *r, dvb_bond_status_v1 *s)
{
    if (!r || !s) return -1;
    if (bond_native_active(r->native)) { bond_native_status(r->native, s); return 0; }
    *s = r->status;
    if (r->active) {
        const bond_stats *b = bond_merger_stats(r->active);
        s->recovered_symbols = b->recovered_symbols; s->missing_packets = b->missing; s->late_frames = b->late;

    }
    return 0;
}
int dvb_bond_rx_status_json(const dvb_bond_rx *r, char *b, size_t capacity)
{
    if (!r || !b || !capacity) return -1;
    dvb_bond_status_v1 s; (void)dvb_bond_rx_status(r, &s);
    int n = snprintf(b, capacity,
        "{\"schema_version\":1,\"delivery_profile\":\"%s\","
        "\"etsi_service_supported\":false,\"source_timing_validated\":false,\"bonding\":%s,\"pair_verified\":%s,\"active\":%s,"
        "\"bond_id\":\"%llu\",\"session\":\"%llu\",\"frequency_hz\":[%llu,%llu],"
        "\"source_bitrate\":%u,\"lane_bitrate\":%u,\"profile\":%u,\"distribution\":%u,\"interleave_groups\":%u,"
        "\"rf\":{\"standard\":\"DVB-T\",\"bandwidth_hz\":40000,\"modulation\":\"QPSK\","
        "\"fft\":2048,\"fec\":\"1/2\",\"guard\":\"1/32\"},"
        "\"tune_partner\":{\"requested\":%s,\"request_id\":\"%llu\",\"receiver_slot\":%u,\"lane\":%u,"
        "\"frequency_hz\":%llu,\"offset_hz\":%lld},\"missing_packets\":%llu,\"recovered_symbols\":%llu,\"null_packets\":%llu}",
        s.distribution == DVB_BOND_DISTRIBUTION_SEPARATE_LANES ? "native_independent_av" : "experimental_private_ts",
        s.discovered_mask ? "true" : "false", s.pair_verified ? "true" : "false", s.active ? "true" : "false",
        (unsigned long long)s.bond_id, (unsigned long long)s.session,
        (unsigned long long)s.frequency_hz[0], (unsigned long long)s.frequency_hz[1],
        s.source_bitrate, s.lane_bitrate, s.profile, (unsigned)s.distribution, s.interleave_groups,
        s.tune_request_sequence ? "true" : "false", (unsigned long long)s.tune_request_sequence,
        s.tune_receiver_slot, s.tune_lane, (unsigned long long)s.tune_frequency_hz,
        (long long)s.tune_offset_hz, (unsigned long long)s.missing_packets, (unsigned long long)s.recovered_symbols, (unsigned long long)s.null_packets);
    return n < 0 || (size_t)n >= capacity ? -1 : n;
}
