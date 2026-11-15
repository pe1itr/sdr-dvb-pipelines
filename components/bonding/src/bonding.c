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

/* Independent project implementation. Design and provenance: docs/design.md
 * and docs/sources.md (S01, S02, S04, S09); not an RFC wire format. */

#include <stdlib.h>
#include <string.h>

struct bond_splitter {
    uint64_t session, base, wire_sequence;
    unsigned group, count, in_symbol;
    int finished;
    uint8_t packets[BOND_MAX_GROUP][BOND_PAYLOAD_SIZE];
    bond_emit_fn emit;
    void *context;
};

typedef struct {
    uint64_t base;
    unsigned count;
    uint32_t present;
    int used;
    uint8_t packets[BOND_MAX_GROUP + 1][BOND_PAYLOAD_SIZE];
} block;

struct bond_merger {
    uint64_t session, next;
    unsigned group, window;
    block *blocks;
    bond_stats stats;
};

static void put64(uint8_t *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(value >> (56 - 8 * i));
}

static uint64_t get64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}

static uint32_t crc32(const uint8_t *frame)
{
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < BOND_FRAME_SIZE; ++i) {
        if (i >= 24 && i < 28) continue;
        crc ^= frame[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void emit_group(bond_splitter *s)
{
    uint8_t parity[BOND_PAYLOAD_SIZE] = {0};
    for (unsigned i = 0; i < s->count; ++i)
        for (unsigned j = 0; j < BOND_PAYLOAD_SIZE; ++j)
            parity[j] ^= s->packets[i][j];
    for (unsigned i = 0; i <= s->count; ++i) {
        uint8_t frame[BOND_FRAME_SIZE] = {0};
        memcpy(frame, "DBND", 4);
        frame[4] = 1;
        frame[5] = (uint8_t)s->count;
        frame[6] = (uint8_t)i;
        put64(frame + 8, s->session);
        put64(frame + 16, s->base);
        memcpy(frame + BOND_HEADER_SIZE,
               i == s->count ? parity : s->packets[i], BOND_PAYLOAD_SIZE);
        uint32_t crc = crc32(frame);
        for (unsigned j = 0; j < 4; ++j)
            frame[24 + j] = (uint8_t)(crc >> (24 - 8 * j));
        s->emit(s->context, (unsigned)(s->wire_sequence++ & 1u), frame);
    }
    s->base += s->count;
    s->count = 0;
}

bond_splitter *bond_splitter_create(uint64_t session, unsigned group_size,
                                    bond_emit_fn emit, void *context)
{
    if (!emit || group_size < 1 || group_size > BOND_MAX_GROUP) return NULL;
    bond_splitter *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->session = session;
    s->group = group_size;
    s->emit = emit;
    s->context = context;
    return s;
}

void bond_splitter_destroy(bond_splitter *s) { free(s); }

int bond_splitter_push(bond_splitter *s, const uint8_t packet[BOND_TS_SIZE])
{
    if (!s || !packet || s->finished || packet[0] != 0x47 ||
        s->base > UINT64_MAX - BOND_MAX_GROUP) return -1;
    memcpy(s->packets[s->count] + s->in_symbol * BOND_TS_SIZE, packet, BOND_TS_SIZE);
    if (++s->in_symbol == BOND_TS_PER_SYMBOL) {
        ++s->count;
        s->in_symbol = 0;
    }
    if (s->count == s->group) emit_group(s);
    return 0;
}

int bond_splitter_finish(bond_splitter *s)
{
    if (!s || s->finished) return -1;
    if (s->in_symbol) {
        while (s->in_symbol < BOND_TS_PER_SYMBOL) {
            uint8_t *p = s->packets[s->count] + s->in_symbol++ * BOND_TS_SIZE;
            memset(p, 0xff, BOND_TS_SIZE);
            p[0] = 0x47; p[1] = 0x1f; p[2] = 0xff; p[3] = 0x10;
        }
        ++s->count;
        s->in_symbol = 0;
    }
    if (s->count) emit_group(s);
    s->finished = 1;
    return 0;
}

bond_merger *bond_merger_create(uint64_t session, unsigned group_size,
                               unsigned window_groups)
{
    return bond_merger_create_at(session, group_size, window_groups, 0);
}

bond_merger *bond_merger_create_at(uint64_t session, unsigned group_size,
                                  unsigned window_groups, uint64_t group_base)
{
    if (group_size < 1 || group_size > BOND_MAX_GROUP ||
        window_groups < 1 || window_groups > BOND_MAX_WINDOW ||
        group_base % group_size || group_base > UINT64_MAX / BOND_TS_PER_SYMBOL) return NULL;
    bond_merger *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->blocks = calloc(window_groups, sizeof(*m->blocks));
    if (!m->blocks) { free(m); return NULL; }
    m->session = session;
    m->group = group_size;
    m->window = window_groups;
    m->next = group_base * BOND_TS_PER_SYMBOL;
    return m;
}

int bond_frame_inspect(const uint8_t *f, size_t length, bond_frame_info *info)
{
    if (!f || !info || length != BOND_FRAME_SIZE || memcmp(f, "DBND", 4) ||
        f[4] != 1 || f[7] || f[5] < 1 || f[5] > BOND_MAX_GROUP || f[6] > f[5]) return -1;
    uint32_t expected = 0;
    for (unsigned i = 0; i < 4; ++i) expected = (expected << 8) | f[24 + i];
    if (crc32(f) != expected || (f[6] < f[5] && f[BOND_HEADER_SIZE] != 0x47)) return -1;
    *info = (bond_frame_info){get64(f + 8), get64(f + 16), f[5], f[6]};
    return 0;
}

int bond_merger_reset(bond_merger *m, uint64_t session, uint64_t base)
{
    if (!m || base % m->group || base > UINT64_MAX / BOND_TS_PER_SYMBOL) return -1;
    memset(m->blocks, 0, m->window * sizeof(*m->blocks));
    memset(&m->stats, 0, sizeof(m->stats));
    m->session = session;
    m->next = base * BOND_TS_PER_SYMBOL;
    return 0;
}

void bond_merger_destroy(bond_merger *m)
{
    if (m) { free(m->blocks); free(m); }
}

int bond_merger_receive(bond_merger *m, const uint8_t *f, size_t length)
{
    if (!m) return -1;
    if (!f || length != BOND_FRAME_SIZE || memcmp(f, "DBND", 4) ||
        f[4] != 1 || f[7] != 0 || f[5] < 1 || f[5] > m->group ||
        f[6] > f[5] || get64(f + 8) != m->session) goto reject;
    uint32_t expected = 0;
    for (unsigned i = 0; i < 4; ++i) expected = (expected << 8) | f[24 + i];
    if (crc32(f) != expected) goto reject;
    uint64_t base = get64(f + 16);
    if (base % m->group || base > UINT64_MAX - BOND_MAX_GROUP ||
        (f[6] < f[5] && f[BOND_HEADER_SIZE] != 0x47)) goto reject;
    uint64_t number = base / m->group;
    uint64_t current = m->next / (m->group * BOND_TS_PER_SYMBOL);
    if (number < current) {
        ++m->stats.late;
        return 0;
    }
    if (number - current >= m->window) {
        ++m->stats.outside_window;
        goto reject;
    }
    block *b = &m->blocks[number % m->window];
    if (!b->used) {
        b->used = 1;
        b->base = base;
        b->count = f[5];
    }
    if (b->base != base || b->count != f[5]) goto reject;
    uint32_t mask = 1u << f[6];
    if (b->present & mask) {
        if (memcmp(b->packets[f[6]], f + BOND_HEADER_SIZE, BOND_PAYLOAD_SIZE)) goto reject;
        ++m->stats.duplicates;
        return 0;
    }
    memcpy(b->packets[f[6]], f + BOND_HEADER_SIZE, BOND_PAYLOAD_SIZE);
    b->present |= mask;
    ++m->stats.accepted;
    return 0;
reject:
    ++m->stats.rejected;
    return -1;
}

int bond_merger_pop(bond_merger *m, uint8_t packet[BOND_TS_SIZE])
{
    if (!m || !packet || m->next == UINT64_MAX) return -1;
    uint64_t symbol = m->next / BOND_TS_PER_SYMBOL;
    unsigned part = (unsigned)(m->next % BOND_TS_PER_SYMBOL);
    unsigned index = (unsigned)(symbol % m->group);
    block *b = &m->blocks[(symbol / m->group) % m->window];
    int available = 0;
    if (b->used && b->base == symbol - index && index < b->count) {
        if (!(b->present & (1u << index)) && (b->present & (1u << b->count))) {
            unsigned missing = 0;
            for (unsigned i = 0; i < b->count; ++i)
                if (!(b->present & (1u << i))) ++missing;
            if (missing == 1) {
                memcpy(b->packets[index], b->packets[b->count], BOND_PAYLOAD_SIZE);
                for (unsigned i = 0; i < b->count; ++i)
                    if (i != index)
                        for (unsigned j = 0; j < BOND_PAYLOAD_SIZE; ++j)
                            b->packets[index][j] ^= b->packets[i][j];
                b->present |= 1u << index;
                ++m->stats.recovered_symbols;
            }
        }
        if (b->present & (1u << index)) {
            memcpy(packet, b->packets[index] + part * BOND_TS_SIZE, BOND_TS_SIZE);
            available = 1;
        }
    }
    if (!available) {
        memset(packet, 0xff, BOND_TS_SIZE);
        packet[0] = 0x47;
        packet[1] = 0x1f;
        packet[2] = 0xff;
        packet[3] = 0x10;
        ++m->stats.missing;
    }
    if (index + 1 == m->group && part + 1 == BOND_TS_PER_SYMBOL)
        memset(b, 0, sizeof(*b));
    ++m->next;
    ++m->stats.emitted;
    return available;
}

const bond_stats *bond_merger_stats(const bond_merger *m)
{
    return m ? &m->stats : NULL;
}
