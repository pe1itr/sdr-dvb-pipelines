#include "bond_discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

typedef struct {
    unsigned calls, slot;
    bond_announcement expected;
} tuning;

static void tune(void *context, unsigned slot, const bond_announcement *expected)
{
    tuning *t = context;
    ++t->calls;
    t->slot = slot;
    t->expected = *expected;
}

int main(void)
{
    bond_announcement a = {
        .bond_id = 123, .session = 456, .revision = 1, .lane = 0,
        .group_size = 8, .own_frequency_hz = 144600000,
        .partner_frequency_hz = 144650000, .bandwidth_hz = 40000,
        .source_bitrate = 51272, .interval_ms = 1000,
        .rf_profile = BOND_RF_DVBT_NARROW_QPSK_2K_1_2_GI_1_32,
    };
    bond_announcement b = a;
    b.lane = 1;
    b.own_frequency_hz = a.partner_frequency_hz;
    b.partner_frequency_hz = a.own_frequency_hz;
    CHECK(bond_announcement_pair(&a, &b));
    CHECK(bond_announcement_pair(&b, &a));
    CHECK(bond_announcement_offset_hz(&a) == 50000);
    CHECK(bond_announcement_offset_hz(&b) == -50000);
    /* The existing receiver's TS parser can feed full PMT descriptors directly.
     * Starting with lane B on receiver zero must tune A on receiver one. */
    {
        tuning t = {0};
        bond_discovery_receiver r;
        uint8_t descriptor[BOND_DISCOVERY_DESCRIPTOR_SIZE];
        bond_announcement parsed;
        CHECK(bond_discovery_receiver_init(&r, tune, &t) == 0);
        CHECK(bond_announcement_descriptor_encode(&b, descriptor) == 0);
        CHECK(bond_announcement_descriptor_decode(&parsed, descriptor, sizeof(descriptor)) == 0);
        CHECK(bond_announcement_pair(&a, &parsed));
        descriptor[0] ^= 1;
        CHECK(bond_discovery_receiver_descriptor(&r, 0, descriptor, sizeof(descriptor)) == -1);
        CHECK(t.calls == 0);
        descriptor[0] ^= 1;
        descriptor[1]--;
        CHECK(bond_discovery_receiver_descriptor(&r, 0, descriptor, sizeof(descriptor)) == -1);
        descriptor[1]++;
        CHECK(bond_discovery_receiver_descriptor(&r, 0, descriptor, sizeof(descriptor) - 1) == -1);
        CHECK(t.calls == 0);
        CHECK(bond_discovery_receiver_descriptor(&r, 0, descriptor, sizeof(descriptor)) == 1);
        CHECK(t.calls == 1 && t.slot == 1 && t.expected.own_frequency_hz == 144600000);
        CHECK(bond_discovery_receiver_descriptor(&r, 0, descriptor, sizeof(descriptor)) == 0);
        CHECK(bond_announcement_descriptor_encode(&a, descriptor) == 0);
        CHECK(bond_discovery_receiver_descriptor(&r, 1, descriptor, sizeof(descriptor)) == 2);
        CHECK(t.calls == 1 && r.pair_verified);
    }
    /* Starting on either lane must request the peer on the other physical slot,
     * once, and only confirm the pair after its reciprocal announcement. */
    for (unsigned first_lane = 0; first_lane < 2; ++first_lane) {
        for (unsigned first_slot = 0; first_slot < 2; ++first_slot) {
            tuning t = {0};
            bond_discovery_receiver r;
            uint8_t first[BOND_ANNOUNCEMENT_SIZE], second[BOND_ANNOUNCEMENT_SIZE];
            const bond_announcement *anchor = first_lane ? &b : &a;
            const bond_announcement *peer = first_lane ? &a : &b;
            CHECK(bond_discovery_receiver_init(&r, tune, &t) == 0);
            CHECK(bond_announcement_encode(anchor, first) == 0);
            CHECK(bond_announcement_encode(peer, second) == 0);
            first[63] ^= 1;
            CHECK(bond_discovery_receiver_receive(&r, first_slot, first, sizeof(first)) == -1);
            CHECK(t.calls == 0);
            first[63] ^= 1;
            CHECK(bond_discovery_receiver_receive(&r, first_slot, first, sizeof(first)) == 1);
            CHECK(t.calls == 1 && t.slot == 1 - first_slot && !r.pair_verified);
            CHECK(t.expected.own_frequency_hz == peer->own_frequency_hz);
            CHECK(t.expected.lane == peer->lane);
            CHECK(bond_discovery_receiver_receive(&r, first_slot, first, sizeof(first)) == 0);
            CHECK(t.calls == 1);
            CHECK(bond_discovery_receiver_receive(&r, 1 - first_slot, first, sizeof(first)) == -1);
            CHECK(!r.pair_verified);
            CHECK(bond_discovery_receiver_receive(&r, 1 - first_slot, second, sizeof(second)) == 2);
            CHECK(t.calls == 1 && r.pair_verified);
        }
    }
    uint8_t wire[BOND_ANNOUNCEMENT_SIZE];
    bond_announcement decoded = {0};
    CHECK(bond_announcement_encode(&b, wire) == 0);
    CHECK(bond_announcement_decode(&decoded, wire, sizeof(wire)) == 0);
    CHECK(bond_announcement_pair(&a, &decoded));
    CHECK(!bond_announcement_pair(&b, &decoded));
    /* Every single-byte corruption is rejected without modifying output. */
    for (unsigned i = 0; i < sizeof(wire); ++i) {
        wire[i] ^= 1;
        CHECK(bond_announcement_decode(&decoded, wire, sizeof(wire)) == -1);
        CHECK(bond_announcement_pair(&a, &decoded));
        wire[i] ^= 1;
    }
    CHECK(bond_announcement_decode(&decoded, wire, sizeof(wire) - 1) == -1);
    b.session++;
    CHECK(!bond_announcement_pair(&a, &b));
    b.session--;
    b.revision++;
    CHECK(!bond_announcement_pair(&a, &b));
    b.revision--;
    b.partner_frequency_hz++;
    CHECK(!bond_announcement_pair(&a, &b));
    b.partner_frequency_hz--;
    b.bond_id++;
    CHECK(!bond_announcement_pair(&a, &b));
    b.bond_id--;
    b.group_size++;
    CHECK(!bond_announcement_pair(&a, &b));
    b.group_size--;
    b.own_frequency_hz = b.partner_frequency_hz;
    CHECK(bond_announcement_encode(&b, wire) == -1);
    b.own_frequency_hz = UINT64_MAX;
    CHECK(bond_announcement_encode(&b, wire) == -1);
    a.own_frequency_hz = 1;
    a.partner_frequency_hz = INT64_MAX;
    CHECK(bond_announcement_encode(&a, wire) == 0);
    CHECK(bond_announcement_offset_hz(&a) == INT64_MAX - 1);
    puts("Discovery: reciprocal +50/-50 kHz, identity and CRC tests passed");
    return 0;
}
