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

#include "bonding.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

typedef struct {
    uint8_t frames[80][BOND_FRAME_SIZE];
    unsigned lanes[80];
    size_t count;
} capture;

static void save(void *context, unsigned lane, const uint8_t *frame)
{
    capture *c = context;
    CHECK(c->count < 80);
    memcpy(c->frames[c->count], frame, BOND_FRAME_SIZE);
    c->lanes[c->count++] = lane;
}

static void source(uint8_t *packet, unsigned sequence)
{
    for (unsigned i = 0; i < BOND_TS_SIZE; ++i)
        packet[i] = (uint8_t)(sequence * 13 + i);
    packet[0] = 0x47;
    packet[1] = (uint8_t)(sequence >> 8);
    packet[2] = (uint8_t)sequence;
    packet[3] = (uint8_t)(0x10 | (sequence & 15));
}

static void roundtrip(unsigned group, unsigned mode)
{
    capture c = {0};
    bond_splitter *s = bond_splitter_create(42, group, save, &c);
    bond_merger *m = bond_merger_create(42, group, 4);
    CHECK(s && m);
    unsigned count = group * BOND_TS_PER_SYMBOL * 2 + 3;
    uint8_t original[BOND_TS_SIZE], actual[BOND_TS_SIZE];
    for (unsigned i = 0; i < count; ++i) {
        source(original, i);
        CHECK(bond_splitter_push(s, original) == 0);
    }
    CHECK(bond_splitter_finish(s) == 0);
    CHECK(bond_splitter_finish(s) == -1);
    CHECK(bond_splitter_push(s, original) == -1);
    /* Reverse delivery tests ordering and tail groups. Mode 1 loses the first
     * source symbol in every block, mode 2 loses all parity, mode 3 a lane. */
    for (size_t j = c.count; j > 0; --j) {
        const uint8_t *f = c.frames[j - 1];
        if (mode == 1 && f[6] == 0) continue;
        if (mode == 2 && f[6] == f[5]) continue;
        if (mode == 3 && c.lanes[j - 1] == 0) continue;
        CHECK(bond_merger_receive(m, f, BOND_FRAME_SIZE) == 0);
        CHECK(bond_merger_receive(m, f, BOND_FRAME_SIZE) == 0);
    }
    unsigned missing = 0;
    for (unsigned i = 0; i < count; ++i) {
        source(original, i);
        int result = bond_merger_pop(m, actual);
        CHECK(result >= 0);
        if (result) CHECK(memcmp(actual, original, BOND_TS_SIZE) == 0);
        else {
            ++missing;
            CHECK(actual[0] == 0x47 && actual[1] == 0x1f && actual[2] == 0xff);
        }
    }
    if (mode != 3 || group == 1) CHECK(missing == 0);
    else CHECK(missing > 0);
    if (mode == 1) CHECK(bond_merger_stats(m)->recovered_symbols == 3);
    CHECK(bond_merger_stats(m)->missing == missing);
    CHECK(bond_merger_stats(m)->emitted == count);
    bond_merger_destroy(m);
    bond_splitter_destroy(s);
}

static void validation_and_deadlines(void)
{
    capture c = {0};
    uint8_t packet[BOND_TS_SIZE];
    bond_splitter *s = bond_splitter_create(42, 4, save, &c);
    bond_merger *m = bond_merger_create(42, 4, 1);
    CHECK(s && m);
    memset(packet, 0, sizeof(packet));
    CHECK(bond_splitter_push(s, packet) == -1);
    for (unsigned i = 0; i < 56; ++i) {
        source(packet, i);
        CHECK(bond_splitter_push(s, packet) == 0);
    }
    CHECK(c.count == 10);
    CHECK(bond_merger_receive(m, c.frames[5], BOND_FRAME_SIZE) == -1);
    CHECK(bond_merger_stats(m)->outside_window == 1);
    CHECK(bond_merger_receive(m, c.frames[0], BOND_FRAME_SIZE - 1) == -1);
    c.frames[0][BOND_HEADER_SIZE + 55] ^= 1;
    CHECK(bond_merger_receive(m, c.frames[0], BOND_FRAME_SIZE) == -1);
    c.frames[0][BOND_HEADER_SIZE + 55] ^= 1;
    bond_merger *wrong = bond_merger_create(43, 4, 1);
    CHECK(wrong);
    CHECK(bond_merger_receive(wrong, c.frames[0], BOND_FRAME_SIZE) == -1);
    bond_merger_destroy(wrong);
    /* Expire a whole block before its data arrives. */
    for (unsigned i = 0; i < 28; ++i) CHECK(bond_merger_pop(m, packet) == 0);
    CHECK(bond_merger_receive(m, c.frames[0], BOND_FRAME_SIZE) == 0);
    CHECK(bond_merger_stats(m)->late == 1);
    for (unsigned i = 5; i < 10; ++i)
        CHECK(bond_merger_receive(m, c.frames[i], BOND_FRAME_SIZE) == 0);
    for (unsigned i = 28; i < 56; ++i) {
        uint8_t expected[BOND_TS_SIZE];
        source(expected, i);
        CHECK(bond_merger_pop(m, packet) == 1);
        CHECK(memcmp(expected, packet, sizeof(packet)) == 0);
    }
    bond_merger_destroy(m);
    bond_splitter_destroy(s);
    CHECK(!bond_merger_create(1, 0, 1));
    CHECK(!bond_merger_create(1, 4, 65));
    CHECK(!bond_splitter_create(1, 17, save, &c));
}

int main(void)
{
    const unsigned groups[] = {1, 4, 8, 16};
    for (unsigned i = 0; i < sizeof(groups) / sizeof(groups[0]); ++i)
        for (unsigned mode = 0; mode < 4; ++mode) roundtrip(groups[i], mode);
    validation_and_deadlines();
    puts("All bonding tests passed");
    return 0;
}
