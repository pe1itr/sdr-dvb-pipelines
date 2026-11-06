// SPDX-License-Identifier: GPL-3.0-only
// Original receiver implementation from ETSI EN 300 401 V2.1.1 (2017-01)
// clauses 10, 11.1, 11.3.2, 12 and ETSI TS 102 563 V2.1.1 (2017-01)
// clauses 5.2, 6 and annexes C/D. No receiver implementation is copied here.
// https://www.etsi.org/deliver/etsi_en/300400_300499/300401/02.01.01_60/en_300401v020101p.pdf
// https://www.etsi.org/deliver/etsi_ts/102500_102599/102563/02.01.01_60/ts_102563v020101p.pdf
#include "dx_msc.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace dab::dx
{
namespace
{
// EN 300 401 table 13: progressively retain another output in each quartet.
// The ordering is the table's 0,4,2,6,1,5,3,7, not the time-interleaver order.
bool retained(unsigned pi, unsigned bit)
{
    constexpr unsigned rank[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    return bit % 4 < 1 + pi / 8 || (bit % 4 == 1 + pi / 8 && rank[bit / 4] < pi % 8);
}
unsigned parity(unsigned v) { return __builtin_parity(v); }

// GF(256), x^8+x^4+x^3+x^2+1; TS 102 563 clause 6.1.
struct Field
{
    std::array<std::uint8_t, 510> exp{};
    std::array<unsigned, 256> log{};
    Field()
    {
        unsigned v = 1;
        for (unsigned i = 0; i < 255; ++i)
        {
            exp[i] = v;
            log[v] = i;
            v <<= 1;
            if (v & 256)
                v ^= 0x11d;
        }
        for (unsigned i = 255; i < 510; ++i)
            exp[i] = exp[i - 255];
    }
    std::uint8_t mul(unsigned a, unsigned b) const { return a && b ? exp[log[a] + log[b]] : 0; }
    std::uint8_t div(unsigned a, unsigned b) const
    {
        return a && b ? exp[log[a] + 255 - log[b]] : 0;
    }
};
const Field gf;
std::array<std::uint8_t, 10> syndromes(const std::array<std::uint8_t, 120> &w)
{
    std::array<std::uint8_t, 10> s{};
    for (unsigned i = 0; i < 10; ++i)
        for (auto b : w)
            s[i] = gf.mul(s[i], gf.exp[i]) ^ b;
    return s;
}
unsigned read_bits(const std::vector<std::uint8_t> &b, unsigned pos, unsigned n)
{
    unsigned out = 0;
    for (unsigned i = 0; i < n; ++i)
        out = (out << 1) | ((b[(pos + i) / 8] >> (7 - (pos + i) % 8)) & 1);
    return out;
}
} // namespace

EepProfile eep_profile(const MuxSubchannelInfo &sub)
{
    EepProfile p;
    if (sub.short_form || sub.protection_option > 1 || sub.protection_level > 3 || !sub.size_cu ||
        unsigned(sub.start_address) + sub.size_cu > 864)
        return p;
    // EN 300 401 tables 18 and 20; levels here are zero-based FIG values.
    constexpr unsigned factors[2][4] = {{12, 8, 6, 4}, {27, 21, 18, 15}};
    unsigned f = factors[sub.protection_option][sub.protection_level];
    if (sub.size_cu % f)
        return p;
    unsigned n = sub.size_cu / f, l1 = 0, l2 = 0, pi1 = 0, pi2 = 0;
    p.bitrate = n * (sub.protection_option ? 32 : 8);
    p.coded_bits = sub.size_cu * 64;
    if (!sub.protection_option)
    {
        switch (sub.protection_level)
        {
        case 0:
            l1 = 6 * n - 3;
            l2 = 3;
            pi1 = 24;
            pi2 = 23;
            break;
        case 1:
            l1 = n == 1 ? 5 : 2 * n - 3;
            l2 = n == 1 ? 1 : 4 * n + 3;
            pi1 = n == 1 ? 13 : 14;
            pi2 = pi1 - 1;
            break;
        case 2:
            l1 = 6 * n - 3;
            l2 = 3;
            pi1 = 8;
            pi2 = 7;
            break;
        case 3:
            l1 = 4 * n - 3;
            l2 = 2 * n + 3;
            pi1 = 3;
            pi2 = 2;
            break;
        }
    }
    else
    {
        constexpr unsigned pi[4] = {10, 6, 4, 2};
        l1 = 24 * n - 3;
        l2 = 3;
        pi1 = pi[sub.protection_level];
        pi2 = pi1 - 1;
    }
    for (unsigned block = 0; block < l1 + l2; ++block)
        for (unsigned bit = 0; bit < 128; ++bit)
            if (retained(block < l1 ? pi1 : pi2, bit % 32))
                p.transmitted_positions.push_back(block * 128 + bit);
    for (unsigned bit = 0; bit < 24; ++bit)
        if (bit % 4 < 2)
            p.transmitted_positions.push_back((l1 + l2) * 128 + bit);
    if (p.transmitted_positions.size() != p.coded_bits)
        return {};
    return p;
}

std::vector<std::uint8_t> decode_msc_word(const float *soft, const EepProfile &p)
{
    if (!soft || !p.bitrate)
        return {};
    const unsigned bits = p.bitrate * 24, steps = bits + 6;
    std::vector<float> mother(steps * 4, 0);
    for (unsigned i = 0; i < p.coded_bits; ++i)
        mother[p.transmitted_positions[i]] = std::isfinite(soft[i]) ? soft[i] : 0;
    // EN 300 401 11.1.1, figure 55. Same shift-register orientation as our FIC.
    // Full-block Viterbi, known zero start/end, with a 64-bit survivor per step.
    // This is ordinary maximum-likelihood decoding, not a new sensitivity claim.
    static const auto signs = []
    {
        constexpr unsigned gen[4] = {0x6d, 0x4f, 0x53, 0x6d};
        std::array<std::array<float, 4>, 128> out{};
        for (unsigned r = 0; r < 128; ++r)
            for (unsigned j = 0; j < 4; ++j)
                out[r][j] = parity(r & gen[j]) ? -1.f : 1.f;
        return out;
    }();
    std::array<float, 64> old{}, next{};
    old.fill(-1e30f);
    old[0] = 0;
    std::vector<std::uint64_t> decisions(steps);
    for (unsigned t = 0; t < steps; ++t)
    {
        float branch[128];
        for (unsigned r = 0; r < 128; ++r)
        {
            branch[r] = 0;
            for (unsigned j = 0; j < 4; ++j)
                branch[r] += signs[r][j] * mother[4 * t + j];
        }
        std::uint64_t choice = 0;
        for (unsigned state = 0; state < 64; ++state)
        {
            float a = old[state >> 1] + branch[state];
            float b = old[(state >> 1) | 32] + branch[state | 64];
            next[state] = std::max(a, b);
            if (b > a)
                choice |= std::uint64_t(1) << state;
        }
        float base = *std::max_element(next.begin(), next.end());
        for (unsigned s = 0; s < 64; ++s)
            old[s] = next[s] - base;
        decisions[t] = choice;
    }
    std::vector<std::uint8_t> decoded(bits), out(bits / 8);
    unsigned state = 0;
    for (unsigned t = steps; t-- > 0;)
    {
        if (t < bits)
            decoded[t] = state & 1;
        state = (state >> 1) | (((decisions[t] >> state) & 1) * 32);
    }
    // EN 300 401 10.1 and 10.3: PRBS restarts for each logical frame.
    unsigned shift = 0x1ff;
    for (unsigned i = 0; i < bits; ++i)
    {
        unsigned bit = ((shift >> 8) ^ (shift >> 4)) & 1;
        shift = ((shift << 1) | bit) & 0x1ff;
        out[i / 8] |= (decoded[i] ^ bit) << (7 - i % 8);
    }
    return out;
}

bool MscSubchannel::configure(const MuxSubchannelInfo &s)
{
    auto next = eep_profile(s);
    if (next.coded_bits == profile_.coded_bits &&
        next.transmitted_positions == profile_.transmitted_positions &&
        start_ == s.start_address * 64U)
        return next.bitrate != 0;
    reset();
    profile_ = std::move(next);
    start_ = s.start_address * 64;
    return profile_.bitrate != 0;
}
void MscSubchannel::reset()
{
    count_ = 0;
    for (auto &h : history_)
        h.clear();
    profile_ = {};
    start_ = 0;
}
std::vector<std::uint8_t> MscSubchannel::push(const float *cif, std::size_t size)
{
    if (!cif || !profile_.bitrate || size != 55296 || start_ + profile_.coded_bits > size)
        return {};
    history_[count_ % 16].assign(cif + start_, cif + start_ + profile_.coded_bits);
    ++count_;
    if (count_ < 16)
        return {};
    // Invert EN 300 401 clause 12/table 21: recover logical time r-15.
    constexpr unsigned delay[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
    std::vector<float> soft(profile_.coded_bits);
    for (unsigned i = 0; i < soft.size(); ++i)
        soft[i] = history_[(count_ - 16 + delay[i % 16]) % 16][i];
    return decode_msc_word(soft.data(), profile_);
}

std::uint16_t crc16(const std::uint8_t *data, std::size_t size)
{
    std::uint16_t crc = 0xffff;
    for (std::size_t i = 0; i < size; ++i)
    {
        crc ^= unsigned(data[i]) << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
    }
    return ~crc;
}
int correct_rs120(std::array<std::uint8_t, 120> &word)
{
    auto s = syndromes(word);
    if (std::all_of(s.begin(), s.end(), [](auto x) { return !x; }))
        return 0;
    // Berlekamp-Massey recurrence for the syndrome sequence, then Chien search.
    // Magnitudes are solved with GF Gaussian elimination (at most 5 unknowns).
    // Parameters, shortening and root ordering: TS 102 563 6.1, RS(120,110), t=5.
    std::array<std::uint8_t, 11> c{}, b{};
    c[0] = b[0] = 1;
    unsigned degree = 0, shift = 1;
    std::uint8_t previous = 1;
    for (unsigned n = 0; n < 10; ++n)
    {
        std::uint8_t d = s[n];
        for (unsigned i = 1; i <= degree; ++i)
            d ^= gf.mul(c[i], s[n - i]);
        if (!d)
        {
            ++shift;
            continue;
        }
        auto saved = c;
        auto scale = gf.div(d, previous);
        for (unsigned i = 0; i + shift < 11; ++i)
            c[i + shift] ^= gf.mul(scale, b[i]);
        if (2 * degree <= n)
        {
            degree = n + 1 - degree;
            b = saved;
            previous = d;
            shift = 1;
        }
        else
            ++shift;
    }
    if (!degree || degree > 5)
        return -1;
    std::vector<unsigned> positions;
    for (unsigned i = 0; i < 120; ++i)
    {
        auto z = gf.exp[(255 - (119 - i)) % 255];
        std::uint8_t v = c[degree];
        for (unsigned j = degree; j-- > 0;)
            v = gf.mul(v, z) ^ c[j];
        if (!v)
            positions.push_back(i);
    }
    if (positions.size() != degree)
        return -1;
    std::uint8_t a[5][6]{};
    for (unsigned r = 0; r < degree; ++r)
    {
        for (unsigned k = 0; k < degree; ++k)
            a[r][k] = gf.exp[((119 - positions[k]) * r) % 255];
        a[r][degree] = s[r];
    }
    for (unsigned k = 0; k < degree; ++k)
    {
        unsigned pivot = k;
        while (pivot < degree && !a[pivot][k])
            ++pivot;
        if (pivot == degree)
            return -1;
        for (unsigned j = 0; j <= degree; ++j)
            std::swap(a[k][j], a[pivot][j]);
        auto scale = a[k][k];
        for (unsigned j = k; j <= degree; ++j)
            a[k][j] = gf.div(a[k][j], scale);
        for (unsigned r = 0; r < degree; ++r)
            if (r != k)
            {
                auto f = a[r][k];
                for (unsigned j = k; j <= degree; ++j)
                    a[r][j] ^= gf.mul(f, a[k][j]);
            }
    }
    auto corrected = word;
    for (unsigned i = 0; i < degree; ++i)
        corrected[positions[i]] ^= a[i][degree];
    auto check = syndromes(corrected);
    if (std::any_of(check.begin(), check.end(), [](auto x) { return x != 0; }))
        return -1;
    word = corrected;
    return degree;
}

void SuperframeDecoder::reset() { frames_.clear(); }
std::vector<AudioUnit> SuperframeDecoder::push(std::vector<std::uint8_t> frame)
{
    std::vector<AudioUnit> units;
    if (frame.empty() || frame.size() % 24 || frame.size() > 576)
    {
        reset();
        return units;
    }
    if (!frames_.empty() && frame.size() != frames_.front().size())
        reset();
    frames_.push_back(std::move(frame));
    if (frames_.size() < 5)
        return units;
    const unsigned columns = frames_[0].size() / 24;
    std::vector<std::uint8_t> bytes;
    for (auto &f : frames_)
        bytes.insert(bytes.end(), f.begin(), f.end());
    bool valid = true;
    unsigned corrections = 0;
    for (unsigned col = 0; col < columns; ++col)
    {
        std::array<std::uint8_t, 120> word{};
        for (unsigned row = 0; row < 120; ++row)
            word[row] = bytes[row * columns + col];
        int corrected = correct_rs120(word);
        if (corrected < 0)
        {
            valid = false;
            continue;
        }
        corrections += corrected;
        for (unsigned row = 0; row < 120; ++row)
            bytes[row * columns + col] = word[row];
    }
    // TS 102 563 5.2: Fire code over bytes 2..10, initial zero, polynomial 0x782f.
    std::uint16_t fire = 0;
    for (unsigned i = 2; i < 11; ++i)
    {
        fire ^= unsigned(bytes[i]) << 8;
        for (int j = 0; j < 8; ++j)
            fire = (fire << 1) ^ ((fire & 0x8000) ? 0x782f : 0);
    }
    valid = valid && fire == unsigned((bytes[0] << 8) | bytes[1]);
    auto h = bytes[2];
    bool dac = h & 0x40, sbr = h & 0x20, ps = h & 8;
    if ((h & 0x80) || (ps && !sbr))
        valid = false;
    const unsigned n = dac ? (sbr ? 3 : 6) : (sbr ? 2 : 4);
    unsigned starts[7]{};
    starts[0] = (24 + (n - 1) * 12 + 7) / 8;
    starts[n] = 110 * columns;
    for (unsigned i = 1; i < n; ++i)
        starts[i] = read_bits(bytes, 24 + 12 * (i - 1), 12);
    for (unsigned i = 0; i < n; ++i)
        if (starts[i] + 2 >= starts[i + 1] || starts[i + 1] > 110 * columns)
            valid = false;
    if (!valid)
    {
        ++rejected;
        frames_.pop_front();
        return units;
    }
    ++good;
    corrected_bytes += corrections;
    for (unsigned i = 0; i < n; ++i)
    {
        unsigned start = starts[i], end = starts[i + 1];
        bool damaged = crc16(bytes.data() + start, end - start - 2) !=
                       unsigned((bytes[end - 2] << 8) | bytes[end - 1]);
        if (damaged)
            ++bad_units;
        AudioUnit unit;
        unit.header = h;
        unit.damaged = damaged;
        if (!damaged)
            unit.bytes.assign(bytes.begin() + start, bytes.begin() + end - 2);
        units.push_back(std::move(unit));
    }
    frames_.clear();
    return units;
}
} // namespace dab::dx
