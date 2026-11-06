#include "dab_detector.hpp"
#include "libdxdabsdr.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <set>
#include <vector>

namespace {

constexpr std::size_t fft_size = 2048;
constexpr std::size_t guard = 504;
constexpr std::size_t null_size = 2656;
constexpr std::size_t symbol_size = fft_size + guard;
constexpr std::size_t frame_size = 196608;

struct Transmitter {
    std::uint8_t main = 0;
    std::uint8_t sub = 0;
    double amplitude = 1.0;
    std::size_t delay = 0;
};

void fft(std::array<std::complex<double>, fft_size>& values, bool inverse)
{
    for (std::size_t i = 1, j = 0; i < fft_size; ++i) {
        std::size_t bit = fft_size >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }
    for (std::size_t length = 2; length <= fft_size; length <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * M_PI / length;
        const std::complex<double> root(std::cos(angle), std::sin(angle));
        for (std::size_t begin = 0; begin < fft_size; begin += length) {
            std::complex<double> factor(1.0, 0.0);
            for (std::size_t offset = 0; offset < length / 2; ++offset) {
                const auto even = values[begin + offset];
                const auto odd = values[begin + offset + length / 2] * factor;
                values[begin + offset] = even + odd;
                values[begin + offset + length / 2] = even - odd;
                factor *= root;
            }
        }
    }
    if (inverse)
        for (auto& value : values) value /= double(fft_size);
}

std::uint8_t pattern_mask(std::uint8_t main)
{
    std::uint8_t index = 0;
    for (unsigned mask = 0; mask < 256; ++mask) {
        if (__builtin_popcount(mask) != 4) continue;
        if (index++ == main) return std::uint8_t(mask);
    }
    return 0;
}

std::array<std::complex<double>, fft_size> tii_wave(const Transmitter& tx)
{
    const auto& prs = dab::mode_i_prs_reference();
    std::array<std::complex<double>, fft_size> prs_spectrum{};
    std::copy(prs.begin(), prs.end(), prs_spectrum.begin());
    fft(prs_spectrum, false);

    std::array<std::complex<double>, fft_size> spectrum{};
    constexpr std::array<int, 4> block_starts = {-768, -384, 1, 385};
    const auto mask = pattern_mask(tx.main);
    for (int group = 0; group < 8; ++group) {
        if (!(mask & (1U << (7 - group)))) continue;
        for (const int block : block_starts) {
            const int carrier = block + 2 * tx.sub + 48 * group;
            const auto first = std::size_t(
                (carrier + int(fft_size)) % int(fft_size));
            const auto second = std::size_t(
                (carrier + 1 + int(fft_size)) % int(fft_size));
            spectrum[first] = prs_spectrum[first];
            spectrum[second] = prs_spectrum[first];
        }
    }
    fft(spectrum, true);
    return spectrum;
}

std::vector<std::complex<float>> make_capture(
    const std::vector<Transmitter>& transmitters, std::size_t frames = 12)
{
    std::vector<std::array<std::complex<double>, fft_size>> waves;
    for (const auto& tx : transmitters) waves.push_back(tii_wave(tx));
    const auto& prs = dab::mode_i_prs_reference();
    std::vector<std::complex<float>> samples(frames * frame_size);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto frame_start = frame * frame_size;
        if (frame % 2 == 0) {
            for (std::size_t n = 0; n < null_size; ++n) {
                std::complex<double> sum{};
                for (std::size_t tx_index = 0;
                     tx_index < transmitters.size(); ++tx_index) {
                    const auto& tx = transmitters[tx_index];
                    if (n < tx.delay) continue;
                    const auto phase = (n - tx.delay + fft_size -
                        (null_size - fft_size)) % fft_size;
                    sum += tx.amplitude * waves[tx_index][phase];
                }
                samples[frame_start + n] = {
                    float(sum.real()), float(sum.imag())};
            }
        }
        for (std::size_t symbol = 0; symbol < 76; ++symbol) {
            const auto start = frame_start + null_size + symbol * symbol_size;
            for (std::size_t n = 0; n < guard; ++n)
                samples[start + n] = prs[fft_size - guard + n];
            std::copy(prs.begin(), prs.end(), samples.begin() + start + guard);
        }
    }
    return samples;
}

std::set<std::string> confirmed_codes(const dab::TiiInfo& result)
{
    std::set<std::string> codes;
    for (const auto& candidate : result.candidates) {
        if (!candidate.confirmed) continue;
        std::ostringstream code;
        code << std::setw(2) << std::setfill('0')
             << unsigned(candidate.main_id) << std::setw(2)
             << unsigned(candidate.sub_id);
        codes.insert(code.str());
    }
    return codes;
}

void append(std::vector<std::complex<float>>& target,
            std::vector<std::complex<float>> source)
{
    target.insert(target.end(), source.begin(), source.end());
}

} // namespace

int main()
{
    const auto single = dab::dx::decode_tii(
        make_capture({{52, 1, 1.0, 0}}), 0, true, 2);
    assert(confirmed_codes(single) == std::set<std::string>{"5201"});

    // Model the observed low-level adjacent-comb product beside 5201. Even
    // though it persists in every active frame, conservative logging must not
    // promote it to a second transmitter.
    const auto adjacent = dab::dx::decode_tii(
        make_capture({{52, 1, 1.0, 0}, {69, 0, 0.01, 0}}), 0, true, 2);
    assert(confirmed_codes(adjacent) == std::set<std::string>{"5201"});

    // Two different-delay patterns on one Sub-ID are intentionally
    // ambiguous for a one-pattern magnitude detector. Phase/margin gating may
    // retain a genuine code, or reject both, but must never invent a third.
    const auto collision = dab::dx::decode_tii(
        make_capture({{21, 16, 1.0, 0}, {35, 16, 0.8, 256}}),
        0, true, 2);
    const auto collision_codes = confirmed_codes(collision);
    assert(std::all_of(collision_codes.begin(), collision_codes.end(),
        [](const auto& code) { return code == "2116" || code == "3516"; }));
    assert(!collision.collisions.empty());
    assert(std::any_of(collision.collisions.begin(), collision.collisions.end(),
        [](const auto& item) {
            return item.sub_id == 16 && item.active_blocks > 4 &&
                std::find(item.possible_main_ids.begin(),
                          item.possible_main_ids.end(), 21) !=
                    item.possible_main_ids.end() &&
                std::find(item.possible_main_ids.begin(),
                          item.possible_main_ids.end(), 35) !=
                    item.possible_main_ids.end();
        }));

    // Four independently decoded blocks model one normal 8-second dwell.
    // 5201 occurs in three blocks and must be confirmed. 6900 occurs in only
    // one internally self-consistent block and must remain a candidate. This
    // is the failure mode seen in the overnight field log: repetition within
    // one bad synchronization interval must not create a persistent TX row.
    std::vector<std::complex<float>> split_capture;
    append(split_capture, make_capture({{52, 1, 1.0, 0}}, 8));
    append(split_capture, make_capture({{52, 1, 1.0, 0}}, 8));
    append(split_capture, make_capture({{69, 0, 1.0, 0}}, 8));
    append(split_capture, make_capture({{52, 1, 1.0, 0}}, 8));
    auto split_settings = dab::default_tii_decode_settings(true, 2);
    split_settings.subdwell_ms = 700.0;
    split_settings.min_confirmed_subdwells = 3;
    const auto split = dab::dx::decode_tii(
        split_capture, 0, split_settings);
    assert(split.subdwells_attempted == 4);
    assert(split.required_subdwells == 3);
    assert(confirmed_codes(split) == std::set<std::string>{"5201"});
    const auto false_code = std::find_if(
        split.candidates.begin(), split.candidates.end(),
        [](const auto& candidate) {
            return candidate.main_id == 69 && candidate.sub_id == 0;
        });
    assert(false_code != split.candidates.end());
    assert(!false_code->confirmed);
    assert(false_code->subdwell_hits == 1);
    assert(false_code->subdwell_observations == 1);

    // Regression for the field-observed false 3715 decode. Main patterns 07
    // (2D) and 37 (8D) differ by one group and combine to collision mask AD.
    // Three subdwells prefer 3715 and one prefers 0715, while another Sub ID
    // supplies persistent, independently measured AD collision evidence.
    // The detector must report the uncertainty instead of allowing 3715 into
    // heard.tsv as a confirmed transmitter.
    std::vector<std::complex<float>> ambiguous_capture;
    for (std::size_t part = 0; part < 3; ++part)
        append(ambiguous_capture, make_capture(
            {{37, 15, 1.0, 0}, {7, 14, 1.0, 0}, {37, 14, 0.8, 256}}, 8));
    append(ambiguous_capture, make_capture({{7, 15, 1.0, 0}}, 8));
    const auto ambiguous = dab::dx::decode_tii(
        ambiguous_capture, 0, split_settings);
    assert(confirmed_codes(ambiguous).count("3715") == 0);
    const auto ambiguous_3715 = std::find_if(
        ambiguous.candidates.begin(), ambiguous.candidates.end(),
        [](const auto& candidate) {
            return candidate.main_id == 37 && candidate.sub_id == 15;
        });
    assert(ambiguous_3715 != ambiguous.candidates.end());
    assert(!ambiguous_3715->confirmed);
    assert(ambiguous_3715->ambiguous);

    std::cout << "libdxdabsdr-tii-synthetic-test: PASS\n";
}
