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
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static void source(uint8_t *p, uint64_t sequence)
{
    for (unsigned j = 0; j < 188; ++j) p[j] = (uint8_t)(sequence * 17 + j * 31);
    p[0] = 0x47; p[1] = 1; p[2] = 0; p[3] = 0x10 | (uint8_t)(sequence & 15);
    memcpy(p + 4, &sequence, sizeof(sequence));
}
static uint64_t advance(uint64_t due, uint64_t *fraction, uint32_t rate)
{
    uint64_t step = UINT64_C(1504000000000) / rate;
    *fraction += UINT64_C(1504000000000) % rate;
    if (*fraction >= rate) { ++step; *fraction -= rate; }
    return due + step;
}
/* fault 1: erase first TS of one source PES per group; 2: lose one whole lane;
 * fault 3: live join after 500 source packets; 4: never drain lane 0 TX queue. */
static void roundtrip(unsigned profile, unsigned depth, unsigned first_lane, unsigned fault, unsigned count)
{
    dvb_bond_config_v1 c; dvb_bond_config_init_v1(&c); c.profile = profile; c.interleave_groups = depth;
    if (profile != 2) c.source_bitrate = 46000;
    c.session = 12345; dvb_bond_tx *t = dvb_bond_tx_create_v1(&c); CHECK(t);
    c.session = 0; dvb_bond_rx *r = dvb_bond_rx_create_v1(&c); CHECK(r);
    uint64_t source_due = 0, source_fraction = 0, pushed = 0, output = 0, anchor = 0;
    int finished = 0, ended[2] = {0,0}, done = 0, anchored = 0; unsigned tunes = 0, pairs = 0;
    uint8_t p[188], expected[188];
    clock_t cpu_start = clock();
    for (uint64_t iterations = 0; iterations < (uint64_t)count * 8 + 100000 && !done; ++iterations) {
        uint64_t now = finished ? UINT64_MAX : source_due;
        for (unsigned lane = 0; lane < 2; ++lane)
            if (!ended[lane] && !(fault == 4 && lane == 0) && dvb_bond_tx_next_due_ns(t, lane) < now)
                now = dvb_bond_tx_next_due_ns(t, lane);
        uint64_t rdue = dvb_bond_rx_next_due_ns(r); if (rdue < now) now = rdue;
        CHECK(now != UINT64_MAX);
        if (!finished && source_due == now) {
            if (pushed == count) { CHECK(!dvb_bond_tx_finish(t)); finished = 1; }
            else {
                source(p, pushed++);
                if (pushed & 1) { CHECK(!dvb_bond_tx_write(t, p, 37)); CHECK(!dvb_bond_tx_write(t, p + 37, 151)); }
                else CHECK(!dvb_bond_tx_push(t, p));
            }
            source_due = advance(source_due, &source_fraction, c.source_bitrate);
        }
        for (unsigned order = 0; order < 2; ++order) {
            unsigned lane = order ? 1 - first_lane : first_lane;
            if (ended[lane] || (fault == 4 && lane == 0)) continue;
            int rc = dvb_bond_tx_pop(t, lane, now, p); CHECK(rc >= 0);
            if (rc == DVB_BOND_END) { ended[lane] = 1; continue; }
            if (rc != DVB_BOND_PACKET || (lane != first_lane && now < UINT64_C(500000000)) ||
                (fault == 2 && lane == 0) || (fault == 3 && pushed < 500)) continue;
            size_t off = ((p[3] >> 4) & 3) == 3 ? 5u + p[4] : 4;
            if (fault == 1 && (p[1] & 0x40) && ((p[1] & 31) == 31) && p[2] == 0xfe &&
                off + 9 + 7 < 188 && !memcmp(p + off + 9, "DBND", 4) && p[off + 15] == 0) continue;
            unsigned slot = 1 - lane; /* Logical lane is deliberately NOT the physical slot. */
            CHECK(!dvb_bond_rx_write(r, slot, p, 19, now));
            CHECK(!dvb_bond_rx_write(r, slot, p + 19, 169, now));
            CHECK(!dvb_bond_rx_push(r, slot, p, now)); /* duplicate packets must be harmless */
            dvb_bond_event_v1 e;
            while (dvb_bond_rx_poll_event(r, &e) == 1) {
                CHECK(e.api_version == 1 && e.struct_size == sizeof(e));
                if (e.type == DVB_BOND_EVENT_TUNE_PARTNER) {
                    ++tunes; CHECK(tunes == 1); CHECK(e.receiver_slot == first_lane);
                    CHECK(e.frequency_hz == c.frequency_hz[1 - first_lane]);
                    CHECK(e.bandwidth_hz == 40000 && e.source_bitrate == c.source_bitrate);
                    CHECK(e.guard_denominator == 32 && e.fec_numerator == 1 && e.fec_denominator == 2);
                    CHECK(e.partner_offset_hz == (first_lane ? -50000 : 50000));
                } else if (e.type == DVB_BOND_EVENT_PAIR_CONFIRMED) { ++pairs; CHECK(pairs == 1); }
            }
        }
        dvb_bond_status_v1 s; CHECK(!dvb_bond_rx_status(r, &s));
        if (s.active && !anchored) { anchor = s.next_source_packet; anchored = 1; }
        int rc = dvb_bond_rx_pop(r, now, p); CHECK(rc >= 0);
        if (rc == DVB_BOND_END) { done = 1; break; }
        if (rc == DVB_BOND_PACKET) {
            source(expected, anchor + output);
            int null = p[1] == 0x1f && p[2] == 0xff;
            if (fault != 2 && fault != 4 && !(fault == 3 && null &&
                output < depth * (profile == 1 ? 8u : 16u) * 7) && memcmp(p, expected, 188)) {
                fprintf(stderr, "mismatch anchor=%llu output=%llu actual=%02x%02x%02x%02x\n",
                        (unsigned long long)anchor, (unsigned long long)output,p[0],p[1],p[2],p[3]);
                CHECK(!memcmp(p, expected, 188));
            }
            ++output;
        }
    }
    CHECK(done && tunes == 1 && output == count - anchor);
    if (fault == 3) CHECK(anchor > 0);
    dvb_bond_status_v1 s; CHECK(!dvb_bond_rx_status(r, &s));
    if (fault == 1) CHECK(s.recovered_symbols > 0);
    if (fault == 2 || fault == 4) CHECK(s.missing_packets > 0 && s.missing_packets < output);
    else if (fault != 3) CHECK(s.missing_packets == 0);
    char json[2048]; CHECK(dvb_bond_rx_status_json(r, json, sizeof(json)) > 0);
    CHECK(strstr(json, "\"schema_version\":1") && strstr(json, "\"requested\":true"));
    CHECK(dvb_bond_rx_status_json(r, json, 5) == -1);
    CHECK(!dvb_bond_tx_status(t, &s));
    if (fault == 4) CHECK(s.queue_drops[0] > 0 && s.queue_drops[1] == 0);
    else CHECK(!s.queue_drops[0] && !s.queue_drops[1]);
    CHECK(!dvb_bond_rx_reset(r)); CHECK(dvb_bond_rx_poll_event(r, &(dvb_bond_event_v1){0}) == 0);
    if (count >= 100000) fprintf(stdout, "sustained: source=%u bit/s, packets=%u, virtual_seconds=%.3f, cpu_seconds=%.3f, drops=0\n",
        c.source_bitrate, count, count * 1504.0 / c.source_bitrate, (clock() - cpu_start) / (double)CLOCKS_PER_SEC);
    dvb_bond_tx_destroy(t); dvb_bond_rx_destroy(r);
}
static void bad_metadata(void)
{
    dvb_bond_config_v1 c; dvb_bond_config_init_v1(&c);
    CHECK(dvb_bond_source_limit_v1(&c) == 50337);
    c.profile = 1; CHECK(dvb_bond_source_limit_v1(&c) == 47541); c.profile = 2;
    c.source_bitrate = 51000; CHECK(!dvb_bond_tx_create_v1(&c)); c.source_bitrate = 50100;
    dvb_bond_tx *t = dvb_bond_tx_create_v1(&c); c.session = 0;
    dvb_bond_rx *r = dvb_bond_rx_create_v1(&c); CHECK(t && r);
    uint8_t p[188]; CHECK(dvb_bond_tx_pop(t, 0, 0, p) == 1);
    CHECK(!dvb_bond_rx_push(r, 0, p, 0));
    uint64_t now = dvb_bond_tx_next_due_ns(t, 0);
    CHECK(dvb_bond_tx_pop(t, 0, now, p) == 1); p[100] ^= 1;
    CHECK(!dvb_bond_rx_push(r, 0, p, now));
    dvb_bond_event_v1 e; CHECK(!dvb_bond_rx_poll_event(r, &e));
    p[100] ^= 1; CHECK(!dvb_bond_rx_push(r, 0, p, now));
    CHECK(dvb_bond_rx_poll_event(r, &e) == 1 && e.type == 1);
    uint32_t random = 17; unsigned cc[3] = {0,0,0};
    for (unsigned i = 0; i < 20000; ++i) {
        for (unsigned j = 0; j < 188; ++j) { random = random * 1664525u + 1013904223u; p[j] = (uint8_t)(random >> 24); }
        unsigned which = i % 3, pid = which == 0 ? 0 : which == 1 ? 4096 : 8190;
        p[0] = 0x47; p[1] = (uint8_t)(pid >> 8); p[2] = (uint8_t)pid;
        if ((i / 3) % 8 == 0) p[1] |= 0x40;
        p[3] = 0x10 | (uint8_t)(cc[which]++ & 15);
        if (which == 2 && (p[1] & 0x40)) memcpy(p + 4, "\0\0\1\xbd\x0a\x83\x80\0\0DBND", 13);
        CHECK(!dvb_bond_rx_push(r, 0, p, now + i));
    }
    dvb_bond_status_v1 s; CHECK(!dvb_bond_rx_status(r, &s) && !s.active);
    CHECK(!dvb_bond_rx_poll_event(r, &e));
    CHECK(dvb_bond_tx_pop(t, 0, now - 1, p) == -1);
    dvb_bond_rx_destroy(r); dvb_bond_tx_destroy(t);
}
static void restart(void)
{
    dvb_bond_config_v1 c; dvb_bond_config_init_v1(&c);
    c.session = 0; dvb_bond_rx *r = dvb_bond_rx_create_v1(&c); CHECK(r);
    uint8_t saved[188];
    for (unsigned session = 1; session <= 2; ++session) {
        c.session = session; dvb_bond_tx *t = dvb_bond_tx_create_v1(&c); CHECK(t);
        uint8_t p[188]; unsigned lane = session - 1;
        CHECK(dvb_bond_tx_pop(t, lane, 0, p) == 1);
        CHECK(!dvb_bond_rx_write(r, lane, p, 188, session * UINT64_C(1000000000)));
        CHECK(dvb_bond_tx_pop(t, lane, dvb_bond_tx_next_due_ns(t, lane), p) == 1);
        CHECK(!dvb_bond_rx_write(r, lane, p, 188, session * UINT64_C(1000000000)));
        if (session == 1) memcpy(saved, p, sizeof(saved));
        dvb_bond_event_v1 e;
        if (session == 2) CHECK(dvb_bond_rx_poll_event(r, &e) == 1 && e.type == DVB_BOND_EVENT_RESTART && e.session == session);
        CHECK(dvb_bond_rx_poll_event(r, &e) == 1 && e.type == 1 && e.session == session);
        CHECK(dvb_bond_rx_poll_event(r, &e) == 0); dvb_bond_tx_destroy(t);
    }
    CHECK(!dvb_bond_rx_push(r, 0, saved, UINT64_C(3000000000)));
    dvb_bond_event_v1 e; CHECK(!dvb_bond_rx_poll_event(r, &e));
    dvb_bond_status_v1 s; CHECK(!dvb_bond_rx_status(r, &s) && s.session == 2);
    dvb_bond_rx_destroy(r);
}
int main(void)
{
    bad_metadata(); restart();
    for (unsigned profile = 1; profile <= 3; ++profile)
        for (unsigned depth = 1; depth <= 2; ++depth)
            for (unsigned side = 0; side < 2; ++side) roundtrip(profile, depth, side, 0, 1027);
    roundtrip(1, 1, 0, 1, 1027); roundtrip(3, 2, 1, 1, 1027);
    roundtrip(1, 1, 1, 2, 1027); roundtrip(3, 2, 1, 4, 1027);
    roundtrip(3, 2, 0, 3, 1027);
    roundtrip(2, 1, 0, 0, 0); roundtrip(2, 1, 1, 0, 1);
    roundtrip(2, 1, 0, 0, 100000);
    puts("TS component, discovery, deadline, live-join and queue-isolation tests passed"); return 0;
}
