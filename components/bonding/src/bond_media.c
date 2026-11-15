#include "bond_media.h"
#include <string.h>

static unsigned word(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static uint32_t crc(const uint8_t *p, size_t n)
{
    uint32_t v = UINT32_MAX;
    while (n--) {
        v ^= (uint32_t)*p++ << 24;
        for (unsigned i = 0; i < 8; ++i) v = (v << 1) ^ (0x04c11db7u & (0u - (v >> 31)));
    }
    return v;
}
static int audio_type(unsigned type, const uint8_t *p, size_t n)
{
    int audio = type == 3 || type == 4 || type == 0x0f || type == 0x11 || type == 0x81 || type == 0x87;
    for (size_t i = 0; i < n;) {
        if (i + 2 > n || i + 2u + p[i + 1] > n) return -1;
        if (type == 6) {
            unsigned tag = p[i];
            if (tag == 0x6a || tag == 0x7a || tag == 0x7b || tag == 0x7c) audio = 1;
            if (tag == 5 && p[i + 1] >= 4 &&
                (!memcmp(p + i + 2, "AC-3", 4) || !memcmp(p + i + 2, "EAC3", 4) ||
                 !memcmp(p + i + 2, "DTS1", 4) || !memcmp(p + i + 2, "DTS2", 4) ||
                 !memcmp(p + i + 2, "DTS3", 4) || !memcmp(p + i + 2, "Opus", 4))) audio = 1;
        }
        i += 2u + p[i + 1];
    }
    return audio;
}
static int complete(bond_media *m, bond_media_section *s, int pat)
{
    const uint8_t *p = s->bytes; size_t n = s->need;
    if (n < 12 || crc(p, n) || (p[1] & 0xf0) != 0xb0) return -1;
    if (!(p[5] & 1)) return 0; /* A future table must not change routing. */
    if (p[6] || p[7]) return -1; /* Single-section, single-program input. */
    if (pat) {
        if (p[0] || (n - 12) % 4) return -1;
        unsigned program = 0, pid = 0;
        for (size_t i = 8; i + 4 <= n - 4; i += 4) {
            if (!word(p + i)) continue; /* Network information entry. */
            if (program) return -1;
            program = word(p + i); pid = word(p + i + 2) & 8191;
        }
        if (!program || pid < 32 || pid == 8191) return -1;
        if (program != m->program || pid != m->pmt_pid) {
            memset(&m->pmt, 0, sizeof(m->pmt)); memset(m->audio, 0, sizeof(m->audio)); m->ready = 0;
        }
        m->program = program; m->pmt_pid = pid;
        memcpy(m->pat_table, p, n); m->pat_length = n;
    } else {
        if (p[0] != 2 || n < 16 || word(p + 3) != m->program) return -1;
        size_t i = 12u + (word(p + 10) & 4095);
        uint8_t audio[8192] = {0};
        if (i > n - 4) return -1;
        while (i < n - 4) {
            if (i + 5 > n - 4) return -1;
            size_t length = word(p + i + 3) & 4095;
            if (i + 5 + length > n - 4) return -1;
            unsigned pid = word(p + i + 1) & 8191;
            int kind = audio_type(p[i], p + i + 5, length);
            if (kind < 0 || pid < 32 || pid == 8191 || pid == m->pmt_pid) return -1;
            audio[pid] = (uint8_t)kind;
            i += 5 + length;
        }
        memcpy(m->audio, audio, sizeof(audio)); m->ready = 1;
        memcpy(m->pmt_table, p, n); m->pmt_length = n;
    }
    return 0;
}
static int bytes(bond_media *m, bond_media_section *s, int pat, const uint8_t *p, size_t n, int continuation)
{
    while (n--) {
        if (!s->count && (*p == 0xff || continuation)) return 0;
        s->bytes[s->count++] = *p++;
        if (s->count == 3) {
            s->need = 3u + (word(s->bytes + 1) & 4095);
            if (s->need < 12 || s->need > sizeof(s->bytes)) { s->count = s->need = 0; return -1; }
        }
        if (s->need && s->count == s->need) {
            int result = complete(m, s, pat);
            s->count = s->need = 0;
            if (result) return -1;
        }
    }
    return 0;
}
int bond_media_route(bond_media *m, const uint8_t p[188])
{
    unsigned pid = ((p[1] & 31) << 8) | p[2], afc = (p[3] >> 4) & 3;
    if ((p[1] & 0x80) || (p[3] & 0xc0) || !afc ||
        ((afc == 2 || afc == 3) && (p[4] > 183 || (p[4] && (p[5] & 0x10) && p[4] < 7)))) return -1;
    int pat = pid == 0;
    if (pat || (m->program && pid == m->pmt_pid)) {
        bond_media_section *s = pat ? &m->pat : &m->pmt;
        if ((afc == 2 || afc == 3) && p[4] && (p[5] & 0x80)) { s->seen = 0; s->count = s->need = 0; }
        if (afc == 2) return 0;
        if (s->seen && (p[3] & 15) == s->cc && !memcmp(s->previous, p, 188)) return 0;
        if (s->seen && (p[3] & 15) != ((s->cc + 1) & 15)) s->count = s->need = 0;
        s->seen = 1; s->cc = p[3] & 15; memcpy(s->previous, p, 188);
        size_t off = afc == 3 ? 5u + p[4] : 4;
        if (off >= 188) return 0;
        if (p[1] & 0x40) {
            size_t pointer = p[off++];
            if (pointer > 188 - off) return -1;
            if (s->count && bytes(m, s, pat, p + off, pointer, 1)) return -1;
            s->count = s->need = 0; off += pointer;
        } else if (!s->count) return 0;
        if (bytes(m, s, pat, p + off, 188 - off, !(p[1] & 0x40))) return -1;
        return 0;
    }
    if (!m->ready && pid >= 32 && pid != 8191) return -1;
    return m->audio[pid] != 0;
}
void bond_media_null_packet(uint8_t p[188], unsigned cc)
{
    memset(p, 0xff, 188); p[0] = 0x47; p[1] = 0x1f; p[2] = 0xff; p[3] = 0x10 | (uint8_t)(cc & 15);
}
