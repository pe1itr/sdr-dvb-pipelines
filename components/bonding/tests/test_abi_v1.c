/* Deliberately compile against the frozen client header, NOT the live header. */
#include "fixtures/dvb_bond_v1_client.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "v1 client contract failed: %s\n", #x); exit(1); } } while (0)
int main(void)
{
    struct { dvb_bond_config_v1 value; uint64_t guard; } c;
    struct { dvb_bond_status_v1 value; uint64_t guard; } s;
    c.guard = s.guard = UINT64_C(0x123456789abcdef0);
    dvb_bond_config_init_v1(&c.value);
    CHECK(c.guard == UINT64_C(0x123456789abcdef0));
    CHECK(c.value.api_version == 1 && c.value.struct_size == sizeof(c.value));
    CHECK(dvb_bond_api_version() == 1 && dvb_bond_source_limit_v1(&c.value) >= c.value.source_bitrate);
    dvb_bond_tx *tx = dvb_bond_tx_create_v1(&c.value); CHECK(tx);
    CHECK(!dvb_bond_tx_status(tx, &s.value));
    CHECK(s.guard == UINT64_C(0x123456789abcdef0));
    CHECK(s.value.struct_size == sizeof(s.value) && s.value.source_bitrate == 50100);
    uint8_t packet[188]; CHECK(dvb_bond_tx_pop(tx, 0, 0, packet) == DVB_BOND_PACKET && packet[0] == 0x47);
    c.value.session = 0; dvb_bond_rx *rx = dvb_bond_rx_create_v1(&c.value); CHECK(rx);
    CHECK(!dvb_bond_rx_write(rx, 0, packet, sizeof(packet), 0));
    uint64_t now = dvb_bond_tx_next_due_ns(tx, 0);
    CHECK(dvb_bond_tx_pop(tx, 0, now, packet) == DVB_BOND_PACKET);
    CHECK(!dvb_bond_rx_write(rx, 0, packet, sizeof(packet), now));
    struct { dvb_bond_event_v1 value; uint64_t guard; } e;
    e.guard = UINT64_C(0x123456789abcdef0);
    CHECK(dvb_bond_rx_poll_event(rx, &e.value) == 1);
    CHECK(e.guard == UINT64_C(0x123456789abcdef0) && e.value.struct_size == sizeof(e.value));
    CHECK(e.value.type == DVB_BOND_EVENT_TUNE_PARTNER && e.value.frequency_hz == 144650000);
    CHECK(!dvb_bond_rx_status(rx, &s.value));
    CHECK(s.guard == UINT64_C(0x123456789abcdef0));
    char json[2048]; CHECK(dvb_bond_rx_status_json(rx, json, sizeof(json)) > 0);
    CHECK(dvb_bond_rx_poll_event(rx, &(dvb_bond_event_v1){0}) == 0);
    dvb_bond_rx_destroy(rx); dvb_bond_tx_destroy(tx);
    puts("Frozen v1 client ABI contract passed"); return 0;
}
