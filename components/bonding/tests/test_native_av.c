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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define STEP(rate) (UINT64_C(1504000000000) / (rate))
static uint32_t crc(const uint8_t *p, size_t n)
{
    uint32_t v = UINT32_MAX;
    while (n--) { v ^= (uint32_t)*p++ << 24; for (unsigned i = 0; i < 8; ++i) v = (v << 1) ^ (0x04c11db7u & (0u - (v >> 31))); }
    return v;
}
static void checksum(uint8_t *p, size_t n)
{ uint32_t c = crc(p, n); for (unsigned i = 0; i < 4; ++i) p[n + i] = (uint8_t)(c >> (24 - i * 8)); }
static unsigned cc[8192];
static unsigned source(uint8_t p[188], unsigned index)
{
    memset(p, 0xff, 188); p[0] = 0x47;
    unsigned pid = 8191, role = 0;
    if (index % 50 < 2) {
        uint8_t pat[16] = {0,0xb0,13,0,1,0xc1,0,0,0,1,0xf0,0};
        uint8_t pmt[26] = {2,0xb0,23,0,1,0xc1,0,0,0xe1,0,0xf0,0,0x1b,0xe1,0,0xf0,0,0x0f,0xe1,1,0xf0,0};
        checksum(pat, 12); checksum(pmt, 22);
        pid = index % 50 ? 4096 : 0;
        p[4] = 0; memcpy(p + 5, pid ? pmt : pat, pid ? sizeof(pmt) : sizeof(pat));
        p[1] = 0x40 | (uint8_t)(pid >> 8); p[2] = (uint8_t)pid; p[3] = 0x10 | (cc[pid]++ & 15);
        return 0;
    }
    if (index % 3 == 2) { pid = 256; role = 1; }
    else if (index % 5 == 4) { pid = 257; role = 2; }
    p[1] = (uint8_t)(pid >> 8) | (role ? 0x40 : 0); p[2] = (uint8_t)pid;
    p[3] = 0x10 | (cc[pid]++ & 15);
    if (role) {
        p[3] |= 0x20; p[4] = 23; p[5] = role == 1 ? 0x10 : 0;
        if (role == 1) {
            uint64_t base = (uint64_t)index * 1504 * 90000 / 50100;
            p[6] = (uint8_t)(base >> 25); p[7] = (uint8_t)(base >> 17); p[8] = (uint8_t)(base >> 9);
            p[9] = (uint8_t)(base >> 1); p[10] = (uint8_t)((base & 1) << 7) | 0x7e; p[11] = 0;
        }
        for (unsigned i = 28; i < 188; ++i) p[i] = (uint8_t)(index * 7 + i);
        p[28] = 0; p[29] = 0; p[30] = 1; p[31] = role == 1 ? 0xe0 : 0xc0;
    }
    return role;
}
static unsigned pidof(const uint8_t *p) { return ((p[1] & 31u) << 8) | p[2]; }
static uint8_t expected[2][200000], merged[2][200000];
static size_t expected_n[2], merged_n[2];
static void collect(const uint8_t p[188], uint8_t data[2][200000], size_t count[2])
{
    unsigned pid = pidof(p); if ((pid != 256 && pid != 257) || !(p[3] & 0x10)) return;
    unsigned lane = pid - 256, off = p[3] & 0x20 ? 5u + p[4] : 4;
    CHECK(count[lane] + 188 - off <= 200000);
    memcpy(data[lane] + count[lane], p + off, 188 - off); count[lane] += 188 - off;
}
static void run(unsigned drop_b, unsigned swap)
{
    memset(cc, 0, sizeof(cc)); memset(expected_n, 0, sizeof(expected_n)); memset(merged_n, 0, sizeof(merged_n));
    dvb_bond_config_v1 c; dvb_bond_config_init_v1(&c);
    c.distribution = DVB_BOND_DISTRIBUTION_SEPARATE_LANES; c.profile = DVB_BOND_PROFILE_NONE;
    c.playout_delay_ns = UINT64_C(500000000);
    char contract[4096]; CHECK(dvb_bond_tx_input_contract_json_v1(&c, contract, sizeof(contract)) > 0);
    CHECK(strstr(contract, "DVB-T bonding lanes"));
    CHECK(!strstr(contract, "protection") && !strstr(contract, "automatic_") && !strstr(contract, "etsi"));
    dvb_bond_tx *t = dvb_bond_tx_create_v1(&c); CHECK(t);
    dvb_bond_config_v1 rc; dvb_bond_config_init_v1(&rc); rc.session = 0; rc.playout_delay_ns = c.playout_delay_ns;
    dvb_bond_rx *r = dvb_bond_rx_create_v1(&rc); CHECK(r); /* No receiver mode setting. */
    unsigned index = 0, ended[2] = {0}, finished = 0, rx_finished = 0, done = 0;
    uint64_t video_arrivals[800]; unsigned video_in = 0, video_out = 0;
    unsigned standalone[2][2] = {{0}}, event_tune = 0, event_pair = 0;
    uint8_t p[188]; uint64_t input_due = 0;
    for (unsigned iteration = 0; iteration < 10000 && !done; ++iteration) {
        uint64_t now = index <= 800 ? input_due : UINT64_MAX;
        for (unsigned lane = 0; lane < 2; ++lane) if (!ended[lane] && dvb_bond_tx_next_due_ns(t, lane) < now) now = dvb_bond_tx_next_due_ns(t, lane);
        if (dvb_bond_rx_next_due_ns(r) < now) now = dvb_bond_rx_next_due_ns(r);
        CHECK(now != UINT64_MAX);
        if (index <= 800 && input_due == now) {
            if (index == 800) { CHECK(!dvb_bond_tx_finish(t)); finished = 1; ++index; }
            else { source(p, index++); collect(p, expected, expected_n); CHECK(!dvb_bond_tx_push(t, p)); }
            input_due += STEP(50100);
        }
        for (unsigned lane = 0; lane < 2; ++lane) if (!ended[lane]) {
            int result = dvb_bond_tx_pop(t, lane, now, p); CHECK(result >= 0);
            if (result == DVB_BOND_END) { CHECK(finished); ended[lane] = 1; }
            if (result != DVB_BOND_PACKET) continue;
            unsigned pid = pidof(p);
            if (pid == 256 && (p[3] & 0x10)) ++standalone[lane][0];
            if (pid == 257 && (p[3] & 0x10)) ++standalone[lane][1];
            CHECK(pid != 8190); /* No private PES transport. */
            if (lane == 1 && (drop_b == 1 || (drop_b == 2 && index > 400))) continue;
            CHECK(!dvb_bond_rx_push(r, lane ^ swap, p, now));
            if (lane == 0 && pid == 256 && (p[3] & 0x10)) video_arrivals[video_in++] = now;
        }
        if (ended[0] && ended[1] && !rx_finished) { CHECK(!dvb_bond_rx_finish(r)); rx_finished = 1; }
        int result = dvb_bond_rx_pop(r, now, p); CHECK(result >= 0);
        if (result == DVB_BOND_PACKET) {
            collect(p, merged, merged_n);
            if (pidof(p) == 256 && (p[3] & 0x10)) {
                CHECK(video_out < video_in);
                CHECK(now >= video_arrivals[video_out++] + c.playout_delay_ns);
            }
        }
        if (result == DVB_BOND_END) done = 1;
        dvb_bond_event_v1 e;
        while (dvb_bond_rx_poll_event(r, &e) == 1) {
            CHECK(e.distribution == 2); CHECK(e.profile == 0);
            event_tune += e.type == DVB_BOND_EVENT_TUNE_PARTNER;
            event_pair += e.type == DVB_BOND_EVENT_PAIR_CONFIRMED;
        }
    }
    CHECK(done); CHECK(standalone[0][0] && !standalone[0][1]); CHECK(standalone[1][1] && !standalone[1][0]);
    CHECK(event_tune == 1); CHECK(event_pair == (drop_b != 1));
    CHECK(expected_n[0] == merged_n[0]); CHECK(!memcmp(expected[0], merged[0], expected_n[0]));
    if (drop_b == 1) CHECK(!merged_n[1]);
    else if (drop_b == 2) { CHECK(merged_n[1] && merged_n[1] < expected_n[1]); CHECK(!memcmp(expected[1], merged[1], merged_n[1])); }
    else { CHECK(expected_n[1] == merged_n[1]); CHECK(!memcmp(expected[1], merged[1], expected_n[1])); }
    dvb_bond_status_v1 s; CHECK(!dvb_bond_rx_status(r, &s));
    CHECK(!s.queue_drops[0] && !s.queue_drops[1] && !s.recovered_symbols); CHECK(s.profile == 0 && s.distribution == 2);
    CHECK(!dvb_bond_rx_reset(r)); CHECK(dvb_bond_rx_pop(r, 0, p) == DVB_BOND_WAIT);
    dvb_bond_tx_destroy(t); dvb_bond_rx_destroy(r);
}
static void advertise(dvb_bond_rx *r, unsigned distribution, unsigned session, uint64_t now)
{
    dvb_bond_config_v1 c; dvb_bond_config_init_v1(&c); c.session = session;
    c.distribution = distribution; c.profile = distribution == 2 ? 0 : 2;
    dvb_bond_tx *t = dvb_bond_tx_create_v1(&c); CHECK(t);
    uint8_t p[188]; memset(cc, 0, sizeof(cc));
    if (distribution == 2) for (unsigned i = 0; i < 3; ++i) { source(p, i); CHECK(!dvb_bond_tx_push(t, p)); }
    for (unsigned i = 0; i < 4; ++i) for (unsigned lane = 0; lane < 2; ++lane) {
        CHECK(dvb_bond_tx_pop(t, lane, now + i * STEP(30160), p) == DVB_BOND_PACKET);
        CHECK(!dvb_bond_rx_push(r, lane, p, now + i * STEP(30160)));
    }
    dvb_bond_tx_destroy(t);
}
static void transitions(void)
{
    dvb_bond_config_v1 c; dvb_bond_config_init_v1(&c); c.session = 0;
    dvb_bond_rx *r = dvb_bond_rx_create_v1(&c); CHECK(r);
    dvb_bond_status_v1 s; dvb_bond_event_v1 e; unsigned restarts = 0;
    advertise(r, 0, 10, 0); CHECK(!dvb_bond_rx_status(r, &s)); CHECK(s.distribution == 0 && s.session == 10);
    advertise(r, 2, 11, UINT64_C(1000000000)); CHECK(!dvb_bond_rx_status(r, &s)); CHECK(s.distribution == 2 && s.session == 11);
    while (dvb_bond_rx_poll_event(r, &e) == 1) restarts += e.type == DVB_BOND_EVENT_RESTART;
    CHECK(restarts == 1);
    advertise(r, 0, 10, UINT64_C(2000000000)); CHECK(!dvb_bond_rx_status(r, &s)); CHECK(s.distribution == 2 && s.session == 11);
    advertise(r, 2, 12, UINT64_C(3000000000)); CHECK(!dvb_bond_rx_status(r, &s)); CHECK(s.session == 12 && s.distribution == 2);
    advertise(r, 2, 11, UINT64_C(4000000000)); CHECK(!dvb_bond_rx_status(r, &s)); CHECK(s.session == 12);
    advertise(r, 0, 13, UINT64_C(5000000000)); CHECK(!dvb_bond_rx_status(r, &s)); CHECK(s.session == 13 && s.distribution == 0);
    advertise(r, 2, 12, UINT64_C(6000000000)); CHECK(!dvb_bond_rx_status(r, &s)); CHECK(s.session == 13 && s.distribution == 0);
    advertise(r, 2, 14, UINT64_C(7000000000)); CHECK(!dvb_bond_rx_status(r, &s)); CHECK(s.session == 14 && s.distribution == 2);
    dvb_bond_rx_destroy(r);
}
int main(void)
{
    run(0,0); run(0,1); run(1,0); run(1,1); run(2,0); transitions();
    dvb_bond_config_v1 c; dvb_bond_config_init_v1(&c); c.distribution = 2;
    /* Shared protection settings cannot add protection to separate lanes. */
    c.interleave_groups = 8;
    dvb_bond_tx *native = dvb_bond_tx_create_v1(&c); CHECK(native);
    dvb_bond_status_v1 status; CHECK(!dvb_bond_tx_status(native, &status));
    CHECK(status.profile == DVB_BOND_PROFILE_NONE && status.interleave_groups == 1);
    dvb_bond_tx_destroy(native);
    for (unsigned profile = 0; profile <= 3; ++profile) {
        c.distribution = 1; c.profile = profile;
        CHECK(!dvb_bond_tx_create_v1(&c)); CHECK(!dvb_bond_rx_create_v1(&c));
        CHECK(!dvb_bond_source_limit_v1(&c));
        char contract[4096]; CHECK(dvb_bond_tx_input_contract_json_v1(&c, contract, sizeof(contract)) == -1);
    }
    puts("native A/V isolation, ES preservation, discovery, swapped slots, video-only and drain passed"); return 0;
}
