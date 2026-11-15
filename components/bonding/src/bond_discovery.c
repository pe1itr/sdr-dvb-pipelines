#include "bond_discovery.h"

/* Own experimental protocol. See docs/discovery.md and docs/sources.md
 * (S01, S06, S09); not an assigned DVB SI descriptor. */

#include <string.h>

static void write_number(uint8_t *p, uint64_t v, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = (uint8_t)(v >> ((bytes - i - 1) * 8));
}

static uint64_t read_number(const uint8_t *p, unsigned bytes)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; ++i) v = (v << 8) | p[i];
    return v;
}

static uint32_t announcement_crc(const uint8_t *p)
{
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < 60; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static int valid(const bond_announcement *a)
{
    return a && a->lane < 2 && a->group_size >= 1 && a->group_size <= BOND_MAX_GROUP
        && a->own_frequency_hz > 0 && a->own_frequency_hz <= INT64_MAX
        && a->partner_frequency_hz > 0 && a->partner_frequency_hz <= INT64_MAX
        && a->own_frequency_hz != a->partner_frequency_hz
        && a->bandwidth_hz > 0 && a->source_bitrate > 0
        && a->interval_ms >= 100 && a->interval_ms <= 60000
        && a->rf_profile == BOND_RF_DVBT_NARROW_QPSK_2K_1_2_GI_1_32;
}

int bond_announcement_encode(const bond_announcement *a,
                             uint8_t wire[BOND_ANNOUNCEMENT_SIZE])
{
    if (!wire || !valid(a)) return -1;
    memset(wire, 0, BOND_ANNOUNCEMENT_SIZE);
    memcpy(wire, "DBNA", 4);
    wire[4] = 1;
    wire[5] = BOND_ANNOUNCEMENT_SIZE;
    wire[6] = (uint8_t)a->lane;
    wire[7] = 2;
    write_number(wire + 8, a->bond_id, 8);
    write_number(wire + 16, a->session, 8);
    write_number(wire + 24, a->revision, 4);
    wire[28] = (uint8_t)a->group_size;
    wire[29] = 1; /* XOR parity, one repair symbol per group. */
    wire[30] = BOND_TS_PER_SYMBOL;
    wire[31] = (uint8_t)a->rf_profile;
    write_number(wire + 32, a->own_frequency_hz, 8);
    write_number(wire + 40, a->partner_frequency_hz, 8);
    write_number(wire + 48, a->bandwidth_hz, 4);
    write_number(wire + 52, a->source_bitrate, 4);
    write_number(wire + 56, a->interval_ms, 4);
    write_number(wire + 60, announcement_crc(wire), 4);
    return 0;
}

int bond_announcement_decode(bond_announcement *a, const uint8_t *wire, size_t length)
{
    if (!a || !wire || length != BOND_ANNOUNCEMENT_SIZE ||
        memcmp(wire, "DBNA", 4) || wire[4] != 1 ||
        wire[5] != BOND_ANNOUNCEMENT_SIZE || wire[7] != 2 ||
        wire[29] != 1 || wire[30] != BOND_TS_PER_SYMBOL ||
        read_number(wire + 60, 4) != announcement_crc(wire)) return -1;
    bond_announcement decoded = {
        .bond_id = read_number(wire + 8, 8),
        .session = read_number(wire + 16, 8),
        .revision = (uint32_t)read_number(wire + 24, 4),
        .lane = wire[6], .group_size = wire[28], .rf_profile = wire[31],
        .own_frequency_hz = read_number(wire + 32, 8),
        .partner_frequency_hz = read_number(wire + 40, 8),
        .bandwidth_hz = (uint32_t)read_number(wire + 48, 4),
        .source_bitrate = (uint32_t)read_number(wire + 52, 4),
        .interval_ms = (uint32_t)read_number(wire + 56, 4),
    };
    if (!valid(&decoded)) return -1;
    *a = decoded;
    return 0;
}

int bond_announcement_pair(const bond_announcement *a, const bond_announcement *b)
{
    return valid(a) && valid(b) && a->lane != b->lane
        && a->bond_id == b->bond_id && a->session == b->session
        && a->revision == b->revision && a->group_size == b->group_size
        && a->rf_profile == b->rf_profile && a->bandwidth_hz == b->bandwidth_hz
        && a->source_bitrate == b->source_bitrate && a->interval_ms == b->interval_ms
        && a->own_frequency_hz == b->partner_frequency_hz
        && a->partner_frequency_hz == b->own_frequency_hz;
}

int bond_announcement_descriptor_encode(const bond_announcement *a,
    uint8_t descriptor[BOND_DISCOVERY_DESCRIPTOR_SIZE])
{
    if (!descriptor || bond_announcement_encode(a, descriptor + 2)) return -1;
    descriptor[0] = BOND_DISCOVERY_DESCRIPTOR_TAG;
    descriptor[1] = BOND_ANNOUNCEMENT_SIZE;
    return 0;
}

int bond_announcement_descriptor_decode(bond_announcement *a,
                                        const uint8_t *descriptor, size_t length)
{
    if (!descriptor || length != BOND_DISCOVERY_DESCRIPTOR_SIZE ||
        descriptor[0] != BOND_DISCOVERY_DESCRIPTOR_TAG ||
        descriptor[1] != BOND_ANNOUNCEMENT_SIZE) return -1;
    return bond_announcement_decode(a, descriptor + 2, length - 2);
}

int64_t bond_announcement_offset_hz(const bond_announcement *a)
{
    return (int64_t)a->partner_frequency_hz - (int64_t)a->own_frequency_hz;
}

int bond_discovery_receiver_init(bond_discovery_receiver *r,
                                 bond_tune_fn tune, void *context)
{
    if (!r || !tune) return -1;
    memset(r, 0, sizeof(*r));
    r->tune = tune;
    r->context = context;
    return 0;
}

int bond_discovery_receiver_receive(bond_discovery_receiver *r,
                                    unsigned slot, const uint8_t *wire, size_t length)
{
    bond_announcement a;
    if (!r || !r->tune || slot > 1 || bond_announcement_decode(&a, wire, length)) return -1;
    if (!r->requested) {
        r->anchor = a;
        r->anchor_slot = slot;
        r->requested = 1;
        bond_announcement partner = a;
        partner.lane = 1 - a.lane;
        partner.own_frequency_hz = a.partner_frequency_hz;
        partner.partner_frequency_hz = a.own_frequency_hz;
        r->tune(r->context, 1 - slot, &partner);
        return 1;
    }
    if (slot == r->anchor_slot) {
        uint8_t expected[BOND_ANNOUNCEMENT_SIZE];
        if (bond_announcement_encode(&r->anchor, expected) ||
            memcmp(expected, wire, sizeof(expected))) return -1;
        return 0;
    }
    if (!bond_announcement_pair(&r->anchor, &a)) return -1;
    r->pair_verified = 1;
    return 2;
}

int bond_discovery_receiver_descriptor(bond_discovery_receiver *r,
                                       unsigned slot, const uint8_t *descriptor,
                                       size_t length)
{
    bond_announcement a;
    if (bond_announcement_descriptor_decode(&a, descriptor, length)) return -1;
    return bond_discovery_receiver_receive(r, slot, descriptor + 2, length - 2);
}
