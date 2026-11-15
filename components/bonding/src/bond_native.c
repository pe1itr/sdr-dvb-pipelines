/* Native, single-program TS remultiplexer. No repair or private media frames. */
#include "bond_native.h"
#include "bond_media.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define ANN_TAG 0x82
#define ANN_SIZE 60u
#define NS_PACKET UINT64_C(1504000000000)
typedef struct {
    uint8_t (*packets)[188];
    uint64_t *ready_ns;
    unsigned head, count;
    uint64_t due, fraction, last, ticks, next_psi;
    unsigned phase, cc[8192];
    uint8_t pat[1024], pmt[1024], emission[1024];
    size_t emission_n;
    size_t pat_n, pmt_n, offset;
    int started;
} native_lane;
struct bond_native {
    dvb_bond_config_v1 cfg;
    dvb_bond_status_v1 status;
    bond_media media[2];
    native_lane lane[2];
    unsigned capacity, logical[2], known[2], turn, main_slot;
    uint8_t identity[ANN_SIZE], retired[16][ANN_SIZE];
    unsigned retired_next;
    dvb_bond_event_v1 events[8];
    unsigned event_head, event_count;
    uint64_t sequence, receive_now;
    unsigned audio_pid, audio_size, audio_start;
    uint8_t audio_bytes[184];
    uint64_t source_index, pcr_index, pcr_value;
    int have_pcr;
    int active, finished, receiving;
};
static unsigned word(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static uint64_t get(const uint8_t *p, unsigned n)
{ uint64_t v = 0; while (n--) v = (v << 8) | *p++; return v; }
static void put(uint8_t *p, uint64_t v, unsigned n)
{ while (n) { p[--n] = (uint8_t)v; v >>= 8; } }
static unsigned pidof(const uint8_t *p) { return ((p[1] & 31u) << 8) | p[2]; }
static uint32_t crc(const uint8_t *p, size_t n)
{
    uint32_t c = UINT32_MAX;
    while (n--) {
        c ^= (uint32_t)*p++ << 24;
        for (unsigned i = 0; i < 8; ++i) c = (c << 1) ^ (0x04c11db7u & (0u - (c >> 31)));
    }
    return c;
}
static void close_section(uint8_t *s, size_t n)
{ s[1] = 0xb0 | (uint8_t)((n - 3) >> 8); s[2] = (uint8_t)(n - 3); put(s + n - 4, crc(s, n - 4), 4); }
static void event(bond_native *n, unsigned type, unsigned slot, unsigned lane)
{
    dvb_bond_event_v1 e = {0};
    e.struct_size = sizeof(e); e.api_version = 1; e.sequence = ++n->sequence;
    e.type = type; e.receiver_slot = slot; e.lane = lane;
    e.bond_id = n->status.bond_id; e.session = n->status.session;
    e.frequency_hz = n->status.frequency_hz[lane]; e.anchor_frequency_hz = n->status.frequency_hz[lane ^ 1];
    e.partner_offset_hz = (int64_t)e.frequency_hz - (int64_t)e.anchor_frequency_hz;
    e.source_bitrate = n->status.source_bitrate; e.lane_bitrate = n->status.lane_bitrate;
    e.distribution = DVB_BOND_DISTRIBUTION_SEPARATE_LANES;
    e.rf_profile = 1; e.bandwidth_hz = 40000; e.guard_denominator = 32; e.fec_numerator = 1; e.fec_denominator = 2;
    if (n->event_count == 8) { ++n->status.event_overflows; return; }
    n->events[(n->event_head + n->event_count++) % 8] = e;
    if (type == DVB_BOND_EVENT_TUNE_PARTNER) {
        n->status.tune_request_sequence = e.sequence; n->status.tune_frequency_hz = e.frequency_hz;
        n->status.tune_offset_hz = e.partner_offset_hz; n->status.tune_receiver_slot = slot; n->status.tune_lane = lane;
    }
}
bond_native *bond_native_create(const dvb_bond_config_v1 *c)
{
    bond_native *n = calloc(1, sizeof(*n)); if (!n) return NULL;
    n->cfg = *c;
    n->cfg.profile = DVB_BOND_PROFILE_NONE; n->cfg.interleave_groups = 1;
    n->capacity = c->tx_queue_frames * 7;
    for (unsigned i = 0; i < 2; ++i) {
        n->lane[i].packets = calloc(n->capacity, 188);
        n->lane[i].ready_ns = calloc(n->capacity, sizeof(uint64_t));
        if (!n->lane[i].packets || !n->lane[i].ready_ns) { bond_native_destroy(n); return NULL; }
    }
    bond_native_reset(n); return n;
}
void bond_native_destroy(bond_native *n)
{ if (n) { free(n->lane[0].packets); free(n->lane[1].packets); free(n->lane[0].ready_ns); free(n->lane[1].ready_ns); free(n); } }
void bond_native_reset(bond_native *n)
{
    uint8_t (*a)[188] = n->lane[0].packets, (*b)[188] = n->lane[1].packets;
    uint64_t *ready[2] = {n->lane[0].ready_ns, n->lane[1].ready_ns};
    dvb_bond_config_v1 c = n->cfg; unsigned capacity = n->capacity; uint64_t sequence = n->sequence;
    memset(n, 0, sizeof(*n)); n->cfg = c; n->capacity = capacity; n->sequence = sequence;
    n->lane[0].packets = a; n->lane[1].packets = b;
    n->lane[0].ready_ns = ready[0]; n->lane[1].ready_ns = ready[1];
    n->status.struct_size = sizeof(n->status); n->status.api_version = 1;
    n->status.distribution = DVB_BOND_DISTRIBUTION_SEPARATE_LANES;
    n->status.interleave_groups = 1;
    n->status.source_bitrate = c.source_bitrate; n->status.lane_bitrate = c.lane_bitrate;
    n->status.bond_id = c.bond_id; n->status.session = c.session;
    memcpy(n->status.frequency_hz, c.frequency_hz, sizeof(c.frequency_hz));
}
void bond_native_deactivate(bond_native *n)
{
    if (!n->active) return;
    memcpy(n->retired[n->retired_next++ % 16], n->identity, ANN_SIZE);
    uint8_t retired[16][ANN_SIZE]; memcpy(retired, n->retired, sizeof(retired));
    unsigned next = n->retired_next;
    bond_native_reset(n); memcpy(n->retired, retired, sizeof(retired)); n->retired_next = next;
}
int bond_native_active(const bond_native *n) { return n && n->active; }
static int enqueue(bond_native *n, unsigned lane, const uint8_t p[188])
{
    native_lane *l = &n->lane[lane];
    if (l->count == n->capacity) { ++n->status.queue_drops[lane]; return -1; }
    unsigned index = (l->head + l->count++) % n->capacity;
    memcpy(l->packets[index], p, 188);
    l->ready_ns[index] = n->receiving ? n->receive_now + n->cfg.playout_delay_ns : 0;
    ++n->status.lane_source_packets[lane]; return 0;
}
void bond_native_note_restart(bond_native *n) { event(n, DVB_BOND_EVENT_RESTART, 0, 0); }
static void announcement(const bond_native *n, unsigned lane, uint8_t p[ANN_SIZE])
{
    memset(p, 0, ANN_SIZE); memcpy(p, "DBNA", 4); p[4] = 4; p[5] = (uint8_t)lane;
    p[6] = DVB_BOND_DISTRIBUTION_SEPARATE_LANES; /* p[7]=no repair */
    put(p + 8, n->cfg.bond_id, 8); put(p + 16, n->cfg.session, 8);
    put(p + 24, n->cfg.frequency_hz[0], 8); put(p + 32, n->cfg.frequency_hz[1], 8);
    put(p + 40, n->cfg.source_bitrate, 4); put(p + 44, n->cfg.lane_bitrate, 4);
    put(p + 48, n->cfg.revision, 4); put(p + 52, n->cfg.announcement_ms, 4);
    /* Four reserved zero bytes. PMT CRC protects the entire descriptor. */
}
/* Keep original descriptors/ES bytes; change only programme composition. */
static int tables(bond_native *n, native_lane *l, const bond_media *a,
                  const bond_media *b, unsigned lane, int receive)
{
    if (!a->ready || !a->pat_length) return 0;
    if (a->audio[word(a->pmt_table + 8) & 8191] && !receive) return -1;
    uint8_t old[1024]; size_t old_n = l->pmt_n; memcpy(old, l->pmt, old_n);
    memcpy(l->pat, a->pat_table, a->pat_length); l->pat_n = a->pat_length;
    const uint8_t *src = a->pmt_table; size_t out = 12;
    memcpy(l->pmt, src, 12); l->pmt[5] = 0xc1 | (uint8_t)((n->cfg.revision & 31) << 1);
    if (!receive && lane == 1) {
        unsigned audio_pid = 8191;
        for (unsigned pid = 32; pid < 8191; ++pid) if (a->audio[pid]) {
            if (audio_pid != 8191) return -1; /* Supported native profile: one audio PID. */
            audio_pid = pid;
        }
        if (audio_pid == 8191) return -1;
        n->audio_pid = audio_pid; put(l->pmt + 8, 0xe000u | audio_pid, 2);
    }
    /* Native announcements are private programme descriptors, never an ES. */
    if (!receive) {
        l->pmt[out++] = ANN_TAG; l->pmt[out++] = ANN_SIZE;
        announcement(n, lane, l->pmt + out); out += ANN_SIZE;
    }
    for (size_t i = 12, end = 12u + (word(src + 10) & 4095); i < end;) {
        size_t len = 2u + src[i + 1]; if (i + len > end) return -1;
        if (src[i] != ANN_TAG || src[i + 1] != ANN_SIZE || memcmp(src + i + 2, "DBNA", 4)) {
            if (out + len > 1020) return -1;
            memcpy(l->pmt + out, src + i, len); out += len;
        }
        i += len;
    }
    put(l->pmt + 10, 0xf000u | (out - 12), 2);
    for (unsigned k = 0; k < (receive && b && b->ready ? 2u : 1u); ++k) {
        const bond_media *m = k ? b : a; const uint8_t *s = m->pmt_table;
        for (size_t i = 12u + (word(s + 10) & 4095); i < m->pmt_length - 4;) {
            size_t len = 5u + (word(s + i + 3) & 4095); unsigned pid = word(s + i + 1) & 8191;
            if (i + len > m->pmt_length - 4 || out + len > 1020) return -1;
            if (receive || m->audio[pid] == lane) { memcpy(l->pmt + out, s + i, len); out += len; }
            i += len;
        }
    }
    l->pmt_n = out + 4;
    if (old_n) {
        /* Receivers must see a new PMT version when audio joins/leaves. */
        unsigned changed = old_n != l->pmt_n || memcmp(old, l->pmt, 5) ||
            (old_n == l->pmt_n && memcmp(old + 6, l->pmt + 6, old_n - 10));
        l->pmt[5] = 0xc1 | (uint8_t)((((old[5] >> 1) + changed) & 31) << 1);
    }
    close_section(l->pmt, l->pmt_n); return 0;
}
static uint64_t source_clock(const bond_native *n)
{
    return (n->pcr_value + (n->source_index - n->pcr_index) * UINT64_C(40608000000) / n->cfg.source_bitrate) % (UINT64_C(8589934592) * 300);
}
static int audio_emit(bond_native *n)
{
    if (!n->audio_size) return 0;
    uint8_t p[188]; memset(p, 0xff, sizeof(p));
    p[0] = 0x47; p[1] = (uint8_t)(n->audio_pid >> 8) | (n->audio_start ? 0x40 : 0);
    p[2] = (uint8_t)n->audio_pid; p[3] = 0x30;
    p[4] = (uint8_t)(183 - n->audio_size); p[5] = 0x10;
    uint64_t value = source_clock(n), base = value / 300; unsigned ext = (unsigned)(value % 300);
    p[6] = (uint8_t)(base >> 25); p[7] = (uint8_t)(base >> 17);
    p[8] = (uint8_t)(base >> 9); p[9] = (uint8_t)(base >> 1);
    p[10] = (uint8_t)((base & 1) << 7) | 0x7e | (uint8_t)(ext >> 8); p[11] = (uint8_t)ext;
    memcpy(p + 188 - n->audio_size, n->audio_bytes, n->audio_size);
    n->audio_size = n->audio_start = 0; return enqueue(n, 1, p);
}
int bond_native_tx_push(bond_native *n, const uint8_t p[188])
{
    if (n->finished || p[0] != 0x47) return -1;
    int lane = bond_media_route(&n->media[0], p); if (lane < 0) return -1;
    ++n->status.input_packets[0]; ++n->source_index; unsigned pid = pidof(p);
    if (pid == 8191) { ++n->status.null_packets; return 0; }
    if (pid == 0 || pid == n->media[0].pmt_pid) {
        for (unsigned i = 0; i < 2; ++i) if (tables(n, &n->lane[i], &n->media[0], NULL, i, 0)) return -1;
        return 0;
    }
    if (pid < 32) { int result = enqueue(n, 0, p); if (enqueue(n, 1, p)) result = -1; return result; }
    if (!n->media[0].ready) return -1;
    if ((p[3] & 0x20) && p[4] >= 7 && (p[5] & 0x10) &&
        pid == (word(n->media[0].pmt_table + 8) & 8191)) {
        n->pcr_value = (((uint64_t)p[6] << 25) | ((uint64_t)p[7] << 17) |
            ((uint64_t)p[8] << 9) | ((uint64_t)p[9] << 1) | (p[10] >> 7)) * 300 +
            (((unsigned)p[10] & 1) << 8) + p[11];
        n->pcr_index = n->source_index; n->have_pcr = 1;
    }
    if (lane) {
        if (!n->have_pcr || pid != n->audio_pid) return -1;
        if (!(p[3] & 0x10)) return 0;
        unsigned offset = p[3] & 0x20 ? 5u + p[4] : 4;
        if (p[1] & 0x40) {
            if (audio_emit(n)) return -1;
            n->audio_start = 1;
        }
        while (offset < 188) {
            unsigned len = 176 - n->audio_size; if (len > 188 - offset) len = 188 - offset;
            memcpy(n->audio_bytes + n->audio_size, p + offset, len); n->audio_size += len; offset += len;
            if (n->audio_size == 176 && audio_emit(n)) return -1;
        }
        return 0;
    }
    return enqueue(n, 0, p);
}
int bond_native_finish(bond_native *n)
{
    if (n->finished) return -1;
    if (!n->receiving && audio_emit(n)) return -1;
    n->finished = 1;
    if (!n->receiving) n->status.total_source_packets = n->status.input_packets[0];
    else event(n, DVB_BOND_EVENT_END, 0, 0);
    return 0;
}
static void section_packet(native_lane *l, unsigned pid, const uint8_t *s, size_t length, uint8_t p[188])
{
    memset(p, 0xff, 188); p[0] = 0x47; p[1] = (uint8_t)(pid >> 8); p[2] = (uint8_t)pid; p[3] = 0x10;
    size_t pos = 4;
    if (!l->offset) { p[1] |= 0x40; p[pos++] = 0; }
    size_t len = length - l->offset; if (len > 188 - pos) len = 188 - pos;
    memcpy(p + pos, s + l->offset, len); l->offset += len;
    if (l->offset == length) { l->offset = 0; ++l->phase; }
}
static int pop(bond_native *n, unsigned lane, uint64_t now, uint32_t rate, uint8_t p[188])
{
    native_lane *l = &n->lane[lane];
    if (l->started && now < l->last) return -1;
    l->last = now;
    if (!l->started) { l->started = 1; l->due = now; }
    if (now < l->due) return 0;
    if (!l->pmt_n) {
        if (n->finished) return DVB_BOND_END;
        bond_media_null_packet(p, 0); goto account;
    }
    if (n->finished && !l->count && (!n->receiving || !n->lane[1].count) && !l->phase) {
        if (n->receiving) n->status.total_source_packets = n->status.output_packets[0];
        return DVB_BOND_END;
    }
    if (!l->phase && l->ticks >= l->next_psi) {
        l->phase = 1;
        /* 500ms monitoring schedule rounded down to TS slots.
         * Do not claim the stricter distribution-application profile. */
        uint64_t ticks = (uint64_t)rate * 500 / 1504000;
        l->next_psi = l->ticks + (ticks ? ticks : 1);
    }
    if (l->phase == 1) section_packet(l, 0, l->pat, l->pat_n, p);
    else if (l->phase == 2) {
        if (!l->offset) { memcpy(l->emission, l->pmt, l->pmt_n); l->emission_n = l->pmt_n; }
        section_packet(l, n->media[n->receiving ? n->main_slot : 0].pmt_pid, l->emission, l->emission_n, p);
    }
    else {
        l->phase = 0;
        native_lane *q = l;
        if (n->receiving) {
            q = &n->lane[n->turn]; n->turn ^= 1;
            if (!q->count || q->ready_ns[q->head] > now) q = &n->lane[n->turn];
        }
        if (q->count && q->ready_ns[q->head] <= now) {
            memcpy(p, q->packets[q->head], 188); q->head = (q->head + 1) % n->capacity; --q->count;
        } else bond_media_null_packet(p, 0);
    }
    if (l->phase == 3) l->phase = 0;
    account:;
    unsigned pid = pidof(p), payload = (p[3] & 0x10) != 0;
    if (payload) p[3] = (p[3] & 0xf0) | (l->cc[pid]++ & 15);
    else p[3] = (p[3] & 0xf0) | ((l->cc[pid] - 1) & 15);
    ++l->ticks; ++n->status.output_packets[lane];
    if (n->receiving) n->status.next_source_packet = n->status.output_packets[0];
    uint64_t step = NS_PACKET / rate; l->fraction += NS_PACKET % rate;
    if (l->fraction >= rate) { ++step; l->fraction -= rate; }
    if (UINT64_MAX - l->due < step) return -1;
    l->due += step; return DVB_BOND_PACKET;
}
int bond_native_tx_pop(bond_native *n, unsigned lane, uint64_t now, uint8_t p[188])
{ return pop(n, lane, now, n->cfg.lane_bitrate, p); }
/* Return a validated version-4 descriptor, otherwise ordinary/non-native TS. */
static const uint8_t *descriptor(const bond_media *m)
{
    if (!m->ready) return NULL;
    const uint8_t *s = m->pmt_table; size_t end = 12u + (word(s + 10) & 4095);
    for (size_t i = 12; i + 2 <= end;) {
        size_t len = s[i + 1] + 2u; if (i + len > end) return NULL;
        if (s[i] == ANN_TAG && len == ANN_SIZE + 2 && !memcmp(s + i + 2, "DBNA", 4) && s[i + 6] == 4) return s + i + 2;
        i += len;
    }
    return NULL;
}
int bond_native_rx_push(bond_native *n, unsigned slot, const uint8_t p[188], uint64_t now)
{
    if (UINT64_MAX - now < n->cfg.playout_delay_ns || n->finished) return -1;
    n->receive_now = now;
    bond_media *m = &n->media[slot]; unsigned pid = pidof(p);
    int parsed = bond_media_route(m, p);
    if (parsed < 0) { if (n->active) ++n->status.malformed; return 0; }
    const uint8_t *a = descriptor(m);
    if (a && pid == m->pmt_pid) {
        uint8_t identity[ANN_SIZE]; memcpy(identity, a, ANN_SIZE); identity[5] = 0;
        uint64_t f0 = get(a + 24, 8), f1 = get(a + 32, 8), session = get(a + 16, 8);
        if (a[5] > 1 || a[6] != DVB_BOND_DISTRIBUTION_SEPARATE_LANES || a[7] ||
            get(a + 56, 4) || !get(a + 8, 8) || !session ||
            (n->cfg.session && session != n->cfg.session) || !f0 || !f1 || f0 > INT64_MAX || f1 > INT64_MAX ||
            (f0 > f1 ? f0 - f1 : f1 - f0) != 50000 || get(a + 44, 4) != 30160 ||
            get(a + 40, 4) < 15040 || get(a + 40, 4) > 60320) return 0;
        for (unsigned i = 0; i < 16; ++i) if (!memcmp(identity, n->retired[i], ANN_SIZE)) return 0;
        if (n->active && (get(n->identity + 8, 8) != get(a + 8, 8) ||
            memcmp(n->identity + 24, a + 24, 16))) return 0;
        if (!n->active || memcmp(n->identity, identity, ANN_SIZE)) {
            int restart = n->active;
            if (restart) {
                memcpy(n->retired[n->retired_next++ % 16], n->identity, ANN_SIZE);
                for (unsigned i = 0; i < 2; ++i) {
                    n->known[i] = 0; n->lane[i].head = n->lane[i].count = 0;
                    n->lane[i].phase = 0; n->lane[i].offset = 0;
                }
            }
            memcpy(n->identity, identity, ANN_SIZE); n->active = n->receiving = 1;
            n->status.active = 1; n->status.discovered_mask = 0; n->status.pair_verified = 0;
            n->status.bond_id = get(a + 8, 8); n->status.session = session;
            n->status.frequency_hz[0] = f0; n->status.frequency_hz[1] = f1;
            n->status.source_bitrate = (uint32_t)get(a + 40, 4);
            n->cfg.revision = (uint32_t)get(a + 48, 4);
            n->lane[0].started = 1; n->lane[0].due = now + n->cfg.playout_delay_ns;
            n->lane[0].last = now; n->lane[0].ticks = n->lane[0].next_psi = 0;
            if (restart) event(n, DVB_BOND_EVENT_RESTART, slot, a[5]);
            event(n, DVB_BOND_EVENT_TUNE_PARTNER, slot ^ 1, a[5] ^ 1);
        }
        if (n->known[slot ^ 1]) {
            const bond_media *other = &n->media[slot ^ 1];
            if (n->logical[slot ^ 1] == a[5] || other->program != m->program || other->pmt_pid != m->pmt_pid) return 0;
            for (unsigned pid_check = 32; pid_check < 8191; ++pid_check)
                if (m->audio[pid_check] && !other->audio[pid_check] &&
                    pid_check == (word(other->pmt_table + 8) & 8191)) return 0;
        }
        n->logical[slot] = a[5]; n->known[slot] = 1;
        n->status.discovered_mask |= 1u << a[5];
        if (n->known[slot ^ 1] && n->logical[slot ^ 1] == n->logical[slot]) return 0;
        if (n->status.discovered_mask == 3 && !n->status.pair_verified) {
            n->status.pair_verified = 1; event(n, DVB_BOND_EVENT_PAIR_CONFIRMED, slot, a[5]);
        }
        unsigned main = n->known[0] && n->logical[0] == 0 ? 0 : 1;
        if (n->known[main] && n->logical[main] == 0) {
            n->main_slot = main;
            unsigned partner = main ^ 1;
            if (tables(n, &n->lane[0], &n->media[main], n->known[partner] ? &n->media[partner] : NULL, 0, 1)) return -1;
            /* turn is queue scheduling only; PAT/PMT PID comes from main. */
        }
        return 0;
    }
    if (!n->active || !n->known[slot] || !a) return 0;
    uint8_t current_identity[ANN_SIZE]; memcpy(current_identity, a, ANN_SIZE); current_identity[5] = 0;
    if (memcmp(current_identity, n->identity, ANN_SIZE)) return 0;
    ++n->status.input_packets[slot];
    if (pid == 8191 || pid == 0 || pid == m->pmt_pid) return 0;
    unsigned lane = n->logical[slot];
    /* Audio lane's duplicated SI and PCR are only for standalone playback. */
    if (lane && (pid < 32 || !m->audio[pid])) return 0;
    if (enqueue(n, lane, p)) return -1;
    return 0;
}
int bond_native_rx_pop(bond_native *n, uint64_t now, uint8_t p[188])
{
    if (!n->active || !n->lane[0].pmt_n) return 0;
    return pop(n, 0, now, n->status.source_bitrate, p);
}
uint64_t bond_native_due(const bond_native *n, unsigned lane)
{ return n->receiving && !n->lane[0].pmt_n ? UINT64_MAX : n->lane[lane].due; }
void bond_native_status(const bond_native *n, dvb_bond_status_v1 *s)
{ *s = n->status; s->ended = n->finished; }
int bond_native_event(bond_native *n, dvb_bond_event_v1 *e)
{
    if (!n->event_count) return 0;
    *e = n->events[n->event_head]; n->event_head = (n->event_head + 1) % 8; --n->event_count; return 1;
}
int bond_native_contract(const dvb_bond_config_v1 *c, char *b, size_t capacity)
{
    int n = snprintf(b, capacity,
        "{\"schema_version\":1,\"contract_owner\":\"dvb_bond_splitter\",\"transport_version\":4,\"revision\":%u,\"session\":\"%llu\",\"mode\":{\"distribution\":2,\"name\":\"separate_lanes\",\"protection\":0,\"protection_name\":\"none\",\"interleave_groups\":1},"
        "\"delivery\":{\"profile\":\"native_independent_av\",\"etsi_service_supported\":false},"
        "\"input\":{\"format\":\"mpegts\",\"packet_size_bytes\":188,\"pacing\":\"cbr\",\"single_program_required\":true,\"pat_pmt_before_media_required\":true,\"preserve_pcr_pts\":true,\"source_null_payload\":\"discarded_remuxed\",\"total_ts_bitrate_bps\":%u,\"minimum_total_ts_bitrate_bps\":15040,\"common_pcr_pts_timeline\":true,\"null_padding_discarded\":true},"
        "\"lanes\":[{\"lane\":0,\"role\":\"video\",\"total_ts_bitrate_bps\":%u},{\"lane\":1,\"role\":\"audio\",\"total_ts_bitrate_bps\":%u}],"
        "\"overhead\":{\"repair_bps\":0,\"private_media_framing_bps\":0,\"discovery\":\"62_byte_pmt_descriptor_per_lane\","
        "\"pat_pmt_period_ms\":500,\"psi_ts_bps_min_per_lane\":6032,\"shared_si_copied_to_both_lanes\":true,\"audio_pcr_piggybacked\":true},"
        "\"budget\":{\"per_lane_media_plus_pcr_plus_si_bps_max\":24128,\"audio_payload_bytes_per_ts_packet\":176,\"scope\":\"one_packet_PAT_and_PMT_at_30160bps\",\"larger_pmt_reduces_budget\":true},"
        "\"transport_limits\":{\"scope\":\"byte_capacity_not_encoder_preset\",\"source_cadence_ceiling_bps\":60320,\"source_cadence_within_limit\":%s,\"av_lane_limits\":null},"
        "\"av_timing_reference\":null,\"input_timing_profile\":null,"
        "\"generator\":{\"encoder_preset_supported\":false,\"automatic_codec_configuration_allowed\":false,\"automatic_ts_timing_configuration_allowed\":false,\"reason\":\"native_remux_requires_independent_output_timing_audit\"},"
        "\"requirements\":[\"current PAT/PMT before media\",\"single programme, one audio PID, clear TS\",\"PCR on video before audio, input timestamps continuous within a session\",\"provide native DVB-SI and PCR/PTS timing\","
        "\"each lane including shared SI and ordinary PCR must fit independently\",\"pace input at declared total TS bitrate\"],"
        "\"merge\":{\"packet_exact\":false,\"video_survives_audio_loss\":true,\"audio_survives_video_loss\":false}}",
        c->revision, (unsigned long long)c->session, c->source_bitrate, c->lane_bitrate, c->lane_bitrate,
        c->source_bitrate >= 15040 && c->source_bitrate <= 60320 ? "true" : "false");
    return n < 0 || (size_t)n >= capacity ? -1 : n;
}
