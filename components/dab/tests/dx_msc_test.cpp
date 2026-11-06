// SPDX-License-Identifier: GPL-3.0-only
// Independent transmitter-side fixtures from EN 300 401 tables 13/18/20/21
// and TS 102 563 5.2/6.1. Decoder failures must not modify a failed RS word.
#include "dx_msc.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <random>
using namespace dab::dx;
namespace
{
unsigned mul(unsigned a, unsigned b)
{
    unsigned r = 0;
    for (; b; b >>= 1)
    {
        if (b & 1)
            r ^= a;
        a <<= 1;
        if (a & 256)
            a ^= 0x11d;
    }
    return r;
}
std::array<std::uint8_t, 120> encode_rs(const std::vector<std::uint8_t> &data)
{
    std::vector<unsigned> g{1};
    unsigned root = 1;
    for (unsigned r = 0; r < 10; ++r)
    {
        std::vector<unsigned> next(g.size() + 1);
        for (unsigned i = 0; i < g.size(); ++i)
        {
            next[i] ^= g[i];
            next[i + 1] ^= mul(g[i], root);
        }
        g = next;
        root = mul(root, 2);
    }
    std::array<std::uint8_t, 120> work{};
    std::copy(data.begin(), data.end(), work.begin());
    for (unsigned i = 0; i < 110; ++i)
    {
        auto value = work[i];
        for (unsigned j = 0; j < g.size(); ++j)
            work[i + j] ^= mul(value, g[j]);
    }
    std::copy(data.begin(), data.end(), work.begin());
    return work;
}
std::vector<float> encode(const std::vector<std::uint8_t> &data, const EepProfile &profile)
{
    std::vector<float> mother;
    unsigned state = 0, prbs = 511;
    // Explicit parity equations, separate from decoder trellis/branch table.
    for (unsigned i = 0; i < data.size() * 8 + 6; ++i)
    {
        unsigned bit = 0;
        if (i < data.size() * 8)
        {
            unsigned scramble = ((prbs >> 8) ^ (prbs >> 4)) & 1;
            prbs = ((prbs << 1) | scramble) & 511;
            bit = ((data[i / 8] >> (7 - i % 8)) & 1) ^ scramble;
        }
        state = (state << 1) | bit;
        for (unsigned taps : {0x6d, 0x4f, 0x53, 0x6d})
            mother.push_back(__builtin_parity(state & taps) ? -1.f : 1.f);
    }
    std::vector<float> soft;
    for (auto at : profile.transmitted_positions)
        soft.push_back(mother[at]);
    return soft;
}
std::vector<std::vector<std::uint8_t>> superframe(bool bad_crc = false)
{
    std::vector<std::uint8_t> data(110, 0);
    data[2] = 0x20; // 32 kHz/SBR, two AUs
    data[3] = 3;
    data[4] = 0x20; // AU 1 starts at byte 50
    for (unsigned i = 5; i < 108; ++i)
        data[i] = i;
    for (auto range : {std::pair<unsigned, unsigned>{5, 50}, {50, 110}})
    {
        auto c = crc16(data.data() + range.first, range.second - range.first - 2);
        data[range.second - 2] = c >> 8;
        data[range.second - 1] = c;
    }
    if (bad_crc)
        data[20] ^= 1;
    std::uint16_t f = 0;
    for (unsigned i = 2; i < 11; ++i)
    {
        f ^= unsigned(data[i]) << 8;
        for (unsigned j = 0; j < 8; ++j)
            f = (f << 1) ^ ((f & 0x8000) ? 0x782f : 0);
    }
    data[0] = f >> 8;
    data[1] = f;
    auto word = encode_rs(data);
    std::vector<std::vector<std::uint8_t>> out;
    for (unsigned i = 0; i < 5; ++i)
        out.emplace_back(word.begin() + i * 24, word.begin() + (i + 1) * 24);
    return out;
}
} // namespace
int main()
{
    const auto *check = reinterpret_cast<const std::uint8_t *>("123456789");
    assert(crc16(check, 9) == 0xd64e);
    // Independent literal puncturing masks from EN 300 401 table 13.
    // Round trips below intentionally share the selected transmission positions;
    // these assertions also check the profile's actual bit order against the spec.
    auto mask_at = [](const EepProfile &p, unsigned start)
    {
        std::uint32_t mask = 0;
        for (auto bit : p.transmitted_positions)
            if (bit >= start && bit < start + 32)
                mask |= 1U << (31 - (bit - start));
        return mask;
    };
    dab::MuxSubchannelInfo golden;
    golden.protection_level = 1;
    golden.size_cu = 8;
    auto special = eep_profile(golden);               // 8 kbit/s EEP-2A exception, table 18.
    assert(mask_at(special, 0) == 0xeeecececU);       // PI 13
    assert(mask_at(special, 5 * 128) == 0xececececU); // PI 12
    golden.protection_level = 2;
    golden.size_cu = 6;
    auto ordinary = eep_profile(golden);
    assert(mask_at(ordinary, 0) == 0xccccccccU);       // PI 8
    assert(mask_at(ordinary, 3 * 128) == 0xccccccc8U); // PI 7
    assert(mask_at(ordinary, 6 * 128) == 0xcccccc00U); // 24-bit tail
    std::mt19937 rng(401);
    for (unsigned errors = 0; errors <= 5; ++errors)
        for (unsigned trial = 0; trial < 40; ++trial)
        {
            std::vector<std::uint8_t> payload(110);
            for (auto &v : payload)
                v = rng();
            auto original = encode_rs(payload), damaged = original;
            for (unsigned e = 0; e < errors; ++e)
                damaged[(trial + e * 23) % 120] ^= 1 + rng() % 255;
            assert(correct_rs120(damaged) == int(errors));
            assert(damaged == original);
        }
    auto invalid = encode_rs(std::vector<std::uint8_t>(110, 42));
    for (unsigned i = 0; i < 6; ++i)
        invalid[7 * i] ^= 23 + i;
    auto saved = invalid;
    assert(correct_rs120(invalid) == -1);
    assert(invalid == saved);
    for (unsigned option = 0; option < 2; ++option)
        for (unsigned level = 0; level < 4; ++level)
            for (unsigned n : {1U, 2U, 8U})
            {
                constexpr unsigned factor[2][4] = {{12, 8, 6, 4}, {27, 21, 18, 15}};
                dab::MuxSubchannelInfo sub;
                sub.size_cu = factor[option][level] * n;
                sub.protection_option = option;
                sub.protection_level = level;
                auto p = eep_profile(sub);
                assert(p.bitrate == n * (option ? 32 : 8));
                std::vector<std::uint8_t> data(p.bitrate * 3);
                for (auto &v : data)
                    v = rng();
                auto soft = encode(data, p);
                assert(decode_msc_word(soft.data(), p) == data);
                // Low-confidence errors should be corrected, with punctures as erasures.
                for (unsigned i = 21; i < soft.size(); i += 211)
                    soft[i] *= -0.05f;
                assert(decode_msc_word(soft.data(), p) == data);
            }
    dab::MuxSubchannelInfo sub;
    sub.size_cu = 72;
    sub.protection_level = 2;
    sub.start_address = 10;
    auto p = eep_profile(sub);
    MscSubchannel m;
    assert(m.configure(sub));
    std::vector<std::vector<std::uint8_t>> payloads(40, std::vector<std::uint8_t>(p.bitrate * 3));
    std::vector<std::vector<float>> words;
    for (auto &payload : payloads)
    {
        for (auto &v : payload)
            v = rng();
        words.push_back(encode(payload, p));
    }
    constexpr unsigned delays[] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
    for (unsigned t = 0; t < 40; ++t)
    {
        std::vector<float> cif(55296);
        for (unsigned i = 0; i < p.coded_bits; ++i)
            if (t >= delays[i % 16])
                cif[sub.start_address * 64 + i] = words[t - delays[i % 16]][i];
        auto decoded = m.push(cif.data(), cif.size());
        if (t < 15)
            assert(decoded.empty());
        else
            assert(decoded == payloads[t - 15]);
    }
    m.reset();
    assert(m.push(nullptr, 0).empty());
    sub.short_form = true;
    assert(!m.configure(sub));
    SuperframeDecoder sf;
    std::vector<AudioUnit> units;
    for (auto &f : superframe())
        units = sf.push(f);
    assert(units.size() == 2 && !units[0].damaged && !units[1].damaged && sf.good == 1);
    for (auto &f : superframe(true))
        units = sf.push(f);
    assert(units.size() == 2 && units[0].damaged && !units[1].damaged && sf.bad_units == 1);
    auto damaged = superframe();
    damaged[1][3] ^= 73;
    for (auto &f : damaged)
        units = sf.push(f);
    assert(units.size() == 2 && sf.corrected_bytes == 1);
    sf.reset();
    for (unsigned i = 0; i < 30; ++i)
        assert(sf.push(std::vector<std::uint8_t>(24, 0xff)).empty());
    std::cout
        << "dx-msc: all EEP profiles, deinterleaving, RS correction, CRC and superframes PASS\n";
}
