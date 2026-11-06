#include "libdxdabsdr.hpp"

#include "dab_detector.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace dab::dx {
namespace {

constexpr std::size_t fft_size = 2048;
constexpr std::size_t guard = 504;
constexpr std::size_t symbol_size = fft_size + guard;
constexpr std::size_t null_size = 2656;
constexpr std::size_t frame_size = 196608;
constexpr double sample_rate = 2048000.0;

void fft(std::array<std::complex<double>, fft_size>& values)
{
    for (std::size_t i = 1, j = 0; i < fft_size; ++i) {
        std::size_t bit = fft_size >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }
    for (std::size_t length = 2; length <= fft_size; length <<= 1) {
        const double angle = -2.0 * M_PI / double(length);
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
}

const std::array<int, 1536>& carrier_map()
{
    static const auto map = [] {
        std::array<int, 1536> result{};
        std::size_t output = 0;
        int permutation = 0;
        for (int index = 1; index < 2048; ++index) {
            permutation = (13 * permutation + 511) % 2048;
            if (permutation >= 256 && permutation <= 1792 &&
                permutation != 1024)
                result[output++] = permutation - 1024;
        }
        return result;
    }();
    return map;
}

const std::array<std::complex<double>, fft_size>& prs_reference_spectrum()
{
    static const auto spectrum = [] {
        std::array<std::complex<double>, fft_size> result{};
        const auto& reference = dab::mode_i_prs_reference();
        std::copy(reference.begin(), reference.end(), result.begin());
        fft(result);
        return result;
    }();
    return spectrum;
}

int parity(unsigned value)
{
    value ^= value >> 16;
    value ^= value >> 8;
    value ^= value >> 4;
    return (0x6996U >> (value & 0x0fU)) & 1U;
}

std::array<float, 3096> depuncture(const float* input)
{
    // EN 300 401 clauses 11.1.2 and 11.2.1. PI=16 is 1110 repeated;
    // PI=15 ends in 1100. A zero metric represents a punctured erasure.
    constexpr std::array<int, 32> pi16 = {
        1,1,1,0, 1,1,1,0, 1,1,1,0, 1,1,1,0,
        1,1,1,0, 1,1,1,0, 1,1,1,0, 1,1,1,0};
    constexpr std::array<int, 32> pi15 = {
        1,1,1,0, 1,1,1,0, 1,1,1,0, 1,1,1,0,
        1,1,1,0, 1,1,1,0, 1,1,1,0, 1,1,0,0};
    constexpr std::array<int, 24> tail = {
        1,1,0,0, 1,1,0,0, 1,1,0,0,
        1,1,0,0, 1,1,0,0, 1,1,0,0};
    std::array<float, 3096> output{};
    std::size_t source = 0;
    for (std::size_t block = 0; block < 24; ++block) {
        const auto& pattern = block < 21 ? pi16 : pi15;
        for (std::size_t subblock = 0; subblock < 4; ++subblock) {
            for (std::size_t bit = 0; bit < 32; ++bit) {
                const auto target = block * 128 + subblock * 32 + bit;
                if (pattern[bit]) output[target] = input[source++];
            }
        }
    }
    for (std::size_t bit = 0; bit < tail.size(); ++bit)
        if (tail[bit]) output[3072 + bit] = input[source++];
    return output;
}

std::array<std::uint8_t, 768> viterbi_decode(
    const std::array<float, 3096>& soft)
{
    constexpr std::array<unsigned, 4> generators = {
        0b1101101U, // a_i, a_i-2, a_i-3, a_i-5, a_i-6
        0b1001111U, // a_i, a_i-1, a_i-2, a_i-3, a_i-6
        0b1010011U, // a_i, a_i-1, a_i-4, a_i-6
        0b1101101U};
    constexpr std::size_t steps = 774;
    constexpr float infinity = std::numeric_limits<float>::infinity();
    std::array<float, 64> previous{}, current{};
    previous.fill(infinity);
    previous[0] = 0.0f;
    std::array<std::array<std::uint8_t, 64>, steps> traceback{};
    for (std::size_t time = 0; time < steps; ++time) {
        current.fill(infinity);
        for (unsigned state = 0; state < 64; ++state) {
            if (!std::isfinite(previous[state])) continue;
            for (unsigned input = 0; input < 2; ++input) {
                const unsigned reg = (state << 1) | input;
                const unsigned next = reg & 0x3fU;
                float metric = previous[state];
                for (std::size_t output = 0; output < 4; ++output) {
                    const int bit = parity(reg & generators[output]);
                    const float expected = bit ? -1.0f : 1.0f;
                    metric -= expected * soft[4 * time + output];
                }
                if (metric < current[next]) {
                    current[next] = metric;
                    traceback[time][next] = std::uint8_t(state);
                }
            }
        }
        const float base = *std::min_element(current.begin(), current.end());
        if (std::isfinite(base))
            for (auto& metric : current) metric -= base;
        previous = current;
    }
    std::array<std::uint8_t, steps> decoded{};
    unsigned state = 0; // six transmitted zero tail bits terminate the trellis
    for (std::size_t time = steps; time-- > 0;) {
        decoded[time] = std::uint8_t(state & 1U);
        state = traceback[time][state];
    }
    std::array<std::uint8_t, 768> result{};
    std::copy_n(decoded.begin(), result.size(), result.begin());
    return result;
}

const std::array<std::uint8_t, 768>& prbs()
{
    static const auto sequence = [] {
        std::array<std::uint8_t, 768> result{};
        std::array<std::uint8_t, 9> shift{};
        shift.fill(1);
        for (auto& bit : result) {
            bit = shift[8] ^ shift[4];
            for (std::size_t index = 8; index > 0; --index)
                shift[index] = shift[index - 1];
            shift[0] = bit;
        }
        return result;
    }();
    return sequence;
}

bool valid_fib_crc(const std::array<std::uint8_t, 32>& fib)
{
    std::uint16_t crc = 0xffff;
    for (std::size_t index = 0; index < 30; ++index) {
        crc ^= std::uint16_t(fib[index]) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? std::uint16_t((crc << 1) ^ 0x1021)
                                 : std::uint16_t(crc << 1);
    }
    const auto transmitted = std::uint16_t(fib[30] << 8) | fib[31];
    return std::uint16_t(~crc) == transmitted;
}

std::array<std::array<std::uint8_t, 32>, 3> decode_codeword(
    const float* punctured)
{
    auto bits = viterbi_decode(depuncture(punctured));
    const auto& scramble = prbs();
    for (std::size_t index = 0; index < bits.size(); ++index)
        bits[index] ^= scramble[index];
    std::array<std::array<std::uint8_t, 32>, 3> fibs{};
    for (std::size_t bit = 0; bit < bits.size(); ++bit)
        fibs[bit / 256][(bit % 256) / 8] |=
            bits[bit] << (7 - bit % 8);
    return fibs;
}

struct IdentityAccumulator {
    std::optional<std::uint16_t> eid;
    std::optional<std::uint8_t> ecc;
    std::map<std::uint16_t, std::string> labels;
    std::map<std::uint8_t, MuxSubchannelInfo> subchannels;
    std::map<std::uint32_t, MuxServiceInfo> services;
    std::map<std::uint32_t, std::string> service_labels;
    FicIdentityEvidence fic;
};

std::string clean_label(const std::uint8_t* bytes)
{
    std::string label(reinterpret_cast<const char*>(bytes), 16);
    while (!label.empty() && (label.back() == '\0' || label.back() == ' '))
        label.pop_back();
    for (auto& value : label) {
        const auto byte = static_cast<unsigned char>(value);
        // The initial backend supports the ASCII subset of the EBU label
        // table. Never emit invalid UTF-8 into JSON or persistent state.
        if (byte < 0x20 || byte >= 0x7f) value = '?';
    }
    return label;
}

std::optional<std::uint16_t> eep_bitrate(
    std::uint16_t size_cu, std::uint8_t option, std::uint8_t level)
{
    constexpr std::array<unsigned, 4> a_factors = {12, 8, 6, 4};
    constexpr std::array<unsigned, 4> b_factors = {27, 21, 18, 15};
    if (level >= 4) return std::nullopt;
    const unsigned factor = option == 0 ? a_factors[level]
        : (option == 1 ? b_factors[level] : 0);
    const unsigned unit = option == 0 ? 8 : (option == 1 ? 32 : 0);
    if (!factor || size_cu % factor) return std::nullopt;
    return std::uint16_t(size_cu / factor * unit);
}

void parse_subchannels(const std::uint8_t* data, std::size_t length,
                       IdentityAccumulator& identity)
{
    std::size_t offset = 1; // FIG 0 extension header
    while (offset + 3 <= length) {
        MuxSubchannelInfo value;
        value.id = data[offset] >> 2;
        value.start_address = std::uint16_t(
            ((data[offset] & 0x03U) << 8) | data[offset + 1]);
        const auto form = data[offset + 2];
        value.short_form = (form & 0x80U) == 0;
        if (value.short_form) {
            // The short-form UEP table is deliberately retained as its ETSI
            // table index. It can be enriched later without guessing a rate.
            if (form & 0x40U) {
                offset += 3;
                continue;
            }
            value.table_index = form & 0x3fU;
            offset += 3;
        } else {
            if (offset + 4 > length) break;
            value.protection_option = (form >> 4) & 0x07U;
            value.protection_level = (form >> 2) & 0x03U;
            value.size_cu = std::uint16_t(
                ((form & 0x03U) << 8) | data[offset + 3]);
            // ETSI EN 300 401 defines only option 0 (EEP-A) and option 1
            // (EEP-B). Values 2..7 are reserved and usually indicate that a
            // weak/corrupt FIG 0/1 happened to pass the FIB CRC. Do not let
            // such a value enter persistent mux metadata.
            if (value.protection_option > 1) {
                offset += 4;
                continue;
            }
            value.bitrate_kbps = eep_bitrate(
                value.size_cu, value.protection_option,
                value.protection_level);
            offset += 4;
        }
        identity.subchannels[value.id] = value;
    }
}

void parse_services(const std::uint8_t* data, std::size_t length,
                    IdentityAccumulator& identity)
{
    const bool long_sid = (data[0] & 0x20U) != 0;
    const std::size_t sid_bytes = long_sid ? 4 : 2;
    std::size_t offset = 1;
    while (offset + sid_bytes + 1 <= length) {
        std::uint32_t sid = 0;
        for (std::size_t index = 0; index < sid_bytes; ++index)
            sid = (sid << 8) | data[offset++];
        const auto description = data[offset++];
        const std::size_t component_count = description & 0x0fU;
        if (offset + 2 * component_count > length) break;
        MuxServiceInfo service;
        service.service_id = sid;
        service.programme = !long_sid;
        if (const auto label = identity.service_labels.find(sid);
            label != identity.service_labels.end())
            service.label = label->second;
        for (std::size_t component_index = 0;
             component_index < component_count; ++component_index) {
            const auto first = data[offset++];
            const auto second = data[offset++];
            MuxServiceComponentInfo component;
            component.index = std::uint8_t(component_index);
            component.transport_mode = first >> 6;
            component.component_type = first & 0x3fU;
            component.primary = (second & 0x02U) != 0;
            component.conditional_access = (second & 0x01U) != 0;
            if (component.transport_mode <= 1)
                component.subchannel_id = second >> 2;
            else if (component.transport_mode == 3)
                component.packet_component_id = std::uint16_t(
                    ((first & 0x3fU) << 6) | (second >> 2));
            service.components.push_back(component);
        }
        identity.services[sid] = std::move(service);
    }
}

MuxMetadata mux_metadata(const IdentityAccumulator& identity)
{
    MuxMetadata result;
    for (const auto& [id, value] : identity.subchannels) {
        (void)id;
        result.subchannels.push_back(value);
    }
    for (const auto& [sid, accumulated] : identity.services) {
        auto value = accumulated;
        if (const auto label = identity.service_labels.find(sid);
            label != identity.service_labels.end())
            value.label = label->second;
        result.services.push_back(std::move(value));
    }
    return result;
}

void parse_fib(const std::array<std::uint8_t, 32>& fib,
               IdentityAccumulator& identity)
{
    std::size_t offset = 0;
    while (offset < 30 && fib[offset] != 0xff) {
        const std::uint8_t header = fib[offset++];
        const unsigned type = header >> 5;
        const std::size_t length = header & 0x1f;
        if (length == 0 || offset + length > 30) break;
        const auto* data = fib.data() + offset;
        const unsigned extension = data[0] & (type == 0 ? 0x1fU : 0x07U);
        ++identity.fic.parsed_figs;
        ++identity.fic.fig_counts[std::uint16_t((type << 8) | extension)];
        if (type == 0 && length >= 3) {
            const bool current_ensemble = (data[0] & 0xc0U) == 0;
            if (extension == 0) {
                identity.eid = std::uint16_t(data[1] << 8) | data[2];
                ++identity.fic.eid_counts[*identity.eid];
            } else if (extension == 1 && current_ensemble)
                parse_subchannels(data, length, identity);
            else if (extension == 2 && current_ensemble)
                parse_services(data, length, identity);
            else if (extension == 9 && length >= 4) {
                identity.ecc = data[2];
                ++identity.fic.ecc_counts[*identity.ecc];
            }
        } else if (type == 1 && length >= 21) {
            const bool current_ensemble = (data[0] & 0x08U) == 0;
            if (extension == 0 && current_ensemble) {
                const auto eid = std::uint16_t(data[1] << 8) | data[2];
                const auto label = clean_label(data + 3);
                identity.labels[eid] = label;
                if (!label.empty()) {
                    // FIG 1/0 carries the ensemble EId as well as its label.
                    // It is protected by the same FIB CRC and is valid EId
                    // evidence even when the weak capture misses FIG 0/0.
                    identity.eid = eid;
                    ++identity.fic.eid_counts[eid];
                    ++identity.fic.label_counts[eid][label];
                }
            } else if (extension == 1 && current_ensemble) {
                const auto sid = std::uint16_t(data[1] << 8) | data[2];
                identity.service_labels[sid] = clean_label(data + 3);
            } else if (extension == 5 && length >= 23 && current_ensemble) {
                const auto sid = (std::uint32_t(data[1]) << 24) |
                    (std::uint32_t(data[2]) << 16) |
                    (std::uint32_t(data[3]) << 8) | data[4];
                identity.service_labels[sid] = clean_label(data + 5);
            }
        }
        offset += length;
    }
}

template <typename Key>
std::optional<Key> unique_most_frequent(
    const std::map<Key, std::size_t>& values)
{
    std::optional<Key> selected;
    std::size_t best = 0;
    bool tied = false;
    for (const auto& [value, count] : values) {
        if (count > best) {
            selected = value;
            best = count;
            tied = false;
        } else if (count == best) {
            tied = true;
        }
    }
    return tied ? std::nullopt : selected;
}

void update_decode_identity(DecodeResult& result,
                            const IdentityAccumulator& identity)
{
    result.ensemble.fic = identity.fic;
    result.ensemble.fic.frames_examined = result.stats.frames_examined;
    result.ensemble.fic.codewords_decoded = result.stats.codewords_decoded;
    result.ensemble.fic.available = result.ensemble.fic.valid_fibs != 0;
    result.ensemble.dqpsk = result.stats.dqpsk;
    const auto eid = unique_most_frequent(identity.fic.eid_counts);
    const auto ecc = unique_most_frequent(identity.fic.ecc_counts);
    if (!eid || !ecc || *ecc == 0) return;
    result.ensemble.decoded = true;
    result.ensemble.ueid = (std::uint32_t(*ecc) << 16) | *eid;
    if (const auto label = identity.labels.find(*eid);
        label != identity.labels.end())
        result.ensemble.label = label->second;
    result.ensemble.metadata = mux_metadata(identity);
}

std::array<std::complex<double>, fft_size> symbol_fft(
    const std::vector<std::complex<float>>& samples, std::size_t useful_start,
    double fine_offset_hz, int carrier_offset, bool inverted)
{
    std::array<std::complex<double>, fft_size> spectrum{};
    const double phase_step = -2.0 * M_PI * fine_offset_hz / sample_rate;
    for (std::size_t index = 0; index < fft_size; ++index) {
        const std::size_t absolute = useful_start + index;
        const auto correction = std::polar(1.0, phase_step * double(absolute));
        spectrum[index] = std::complex<double>(samples[absolute]) * correction;
    }
    fft(spectrum);
    std::array<std::complex<double>, fft_size> corrected{};
    for (int carrier = -768; carrier <= 768; ++carrier) {
        if (carrier == 0) continue;
        const int observed = inverted ? -carrier + carrier_offset
                                      : carrier + carrier_offset;
        const auto source = std::size_t((observed + int(fft_size)) % int(fft_size));
        const auto target = std::size_t((carrier + int(fft_size)) % int(fft_size));
        corrected[target] = inverted ? std::conj(spectrum[source])
                                     : spectrum[source];
    }
    return corrected;
}

struct FrameResult {
    std::vector<std::array<std::uint8_t, 32>> fibs;
    std::size_t codewords = 0;
    DqpskQuality dqpsk;
};

double log_sum_exp(const std::array<double, 4>& values)
{
    const double maximum = *std::max_element(values.begin(), values.end());
    double sum = 0.0;
    for (const double value : values) sum += std::exp(value - maximum);
    return maximum + std::log(sum);
}

double log_sum_exp(double first, double second)
{
    const double maximum = std::max(first, second);
    return maximum + std::log(std::exp(first - maximum) +
                              std::exp(second - maximum));
}

const std::array<int, fft_size>& logical_carrier_indices()
{
    static const auto indices = [] {
        std::array<int, fft_size> result{};
        result.fill(-1);
        const auto& map = carrier_map();
        for (std::size_t logical = 0; logical < map.size(); ++logical)
            result[std::size_t(map[logical] + int(fft_size / 2))] =
                int(logical);
        return result;
    }();
    return indices;
}

// Original implementation of the non-iterative dominant-sub-trellis idea in
// W.J. van Houtum, "Two-dimensional block-based reception for differentially
// encoded OFDM systems", TU/e, 2012, sections 6.2--6.4 and 7.3, eq. 7.18.
// https://doi.org/10.6100/IR734135 (dab-monitor/docs/734135.pdf).
// Bibliographic title verified against the locally retained thesis. No
// source code from the thesis or a third-party receiver was available or
// copied. Each block contains eight physically adjacent Mode-I carriers and
// PRS plus the three FIC observations. The known PRS phase makes the initial
// differential state one; exp(-j*n*pi/4) maps pi/4-DE-QPSK to DE-QPSK.
FrameResult decode_frame_two_dimensional(
    const std::vector<std::complex<float>>& samples, std::size_t null_start,
    double fine_offset_hz, int carrier_offset, bool inverted)
{
    FrameResult result;
    const std::size_t prs_start = null_start + null_size + guard;
    if (prs_start + 3 * symbol_size + fft_size > samples.size()) return result;

    std::array<std::array<std::complex<double>, fft_size>, 4> spectra{};
    for (std::size_t symbol = 0; symbol < spectra.size(); ++symbol)
        spectra[symbol] = symbol_fft(
            samples, prs_start + symbol * symbol_size, fine_offset_hz,
            carrier_offset, inverted);

    const auto& reference = prs_reference_spectrum();
    const auto& logical_indices = logical_carrier_indices();
    std::array<float, 9216> soft{};
    constexpr std::size_t carriers_per_block = 8;
    constexpr std::size_t observations = 4;
    constexpr double minimum_variance = 1.0e-3;
    constexpr double maximum_soft_metric = 50.0;

    const auto process_block = [&](int first_carrier) {
        std::array<std::array<std::complex<double>, observations>,
                   carriers_per_block> received{};
        double power = 0.0;
        for (std::size_t local = 0; local < carriers_per_block; ++local) {
            const int carrier = first_carrier + int(local);
            const auto bin = std::size_t(
                (carrier + int(fft_size)) % int(fft_size));
            const double reference_magnitude = std::abs(reference[bin]);
            if (reference_magnitude <= 1.0e-12) return;
            const auto remove_prs_phase =
                std::conj(reference[bin] / reference_magnitude);
            for (std::size_t symbol = 0; symbol < observations; ++symbol) {
                received[local][symbol] = spectra[symbol][bin] *
                    remove_prs_phase *
                    std::polar(1.0, -double(symbol) * M_PI / 4.0);
                power += std::norm(received[local][symbol]);
            }
        }
        const double gain = std::sqrt(
            power / double(carriers_per_block * observations));
        if (gain <= 1.0e-12) return;
        for (auto& carrier : received)
            for (auto& value : carrier) value /= gain;

        // Max-log-MAP phase estimator (thesis equation 7.18). Its ordering is
        // independent of channel gain and noise variance.
        int selected_subtrellis = 0;
        double selected_score = -std::numeric_limits<double>::infinity();
        for (int subtrellis = 0; subtrellis < 8; ++subtrellis) {
            const auto rotation = std::polar(
                1.0, -double(subtrellis) * M_PI / 16.0);
            double score = 0.0;
            for (const auto& carrier : received) {
                for (const auto& value : carrier) {
                    const auto rotated = value * rotation;
                    score += std::max(std::abs(rotated.real()),
                                      std::abs(rotated.imag()));
                }
            }
            if (score > selected_score) {
                selected_score = score;
                selected_subtrellis = subtrellis;
            }
        }

        const double base_phase =
            double(selected_subtrellis) * M_PI / 16.0;
        std::array<std::complex<double>, 4> states{};
        std::array<std::complex<double>, 4> symbols{};
        for (int quadrant = 0; quadrant < 4; ++quadrant) {
            states[std::size_t(quadrant)] = std::polar(
                1.0, base_phase + quadrant * M_PI / 2.0);
            symbols[std::size_t(quadrant)] = std::polar(
                1.0, quadrant * M_PI / 2.0);
        }

        // Estimate the per-real-component noise variance from residual energy
        // to the selected four-state sub-trellis. The small lower bound only
        // prevents infinite metrics for a synthetic/noiseless block.
        double residual = 0.0;
        for (const auto& carrier : received) {
            for (const auto& value : carrier) {
                double nearest = std::numeric_limits<double>::infinity();
                for (const auto& state : states)
                    nearest = std::min(nearest, std::norm(value - state));
                residual += nearest;
            }
        }
        const double variance = std::max(
            minimum_variance,
            residual / (2.0 * carriers_per_block * observations));

        for (std::size_t local = 0; local < carriers_per_block; ++local) {
            const int physical = first_carrier + int(local);
            const int logical = logical_indices[
                std::size_t(physical + int(fft_size / 2))];
            if (logical < 0) continue;
            for (std::size_t transition = 1; transition < observations;
                 ++transition) {
                std::array<double, 4> symbol_log_probability{};
                for (std::size_t symbol = 0; symbol < symbols.size(); ++symbol) {
                    std::array<double, 4> state_log_probability{};
                    for (std::size_t state = 0; state < states.size(); ++state) {
                        const double error = std::norm(
                            received[local][transition - 1] - states[state]) +
                            std::norm(received[local][transition] -
                                      states[state] * symbols[symbol]);
                        state_log_probability[state] =
                            -error / (2.0 * variance);
                    }
                    symbol_log_probability[symbol] =
                        log_sum_exp(state_log_probability);
                }

                // EN 300 401 orders the I bit plane before the Q bit plane.
                // After the pi/4 transform the Gray labels are:
                // phase 0=00, pi/2=10, pi=11, 3pi/2=01.
                const double i_zero = log_sum_exp(
                    symbol_log_probability[0], symbol_log_probability[3]);
                const double i_one = log_sum_exp(
                    symbol_log_probability[1], symbol_log_probability[2]);
                const double q_zero = log_sum_exp(
                    symbol_log_probability[0], symbol_log_probability[1]);
                const double q_one = log_sum_exp(
                    symbol_log_probability[2], symbol_log_probability[3]);
                const auto base = (transition - 1) * 3072;
                soft[base + std::size_t(logical)] = float(std::clamp(
                    i_zero - i_one, -maximum_soft_metric,
                    maximum_soft_metric));
                soft[base + 1536 + std::size_t(logical)] = float(std::clamp(
                    q_zero - q_one, -maximum_soft_metric,
                    maximum_soft_metric));
            }
        }
    };

    for (int first = -768; first <= -8; first += 8) process_block(first);
    for (int first = 1; first <= 761; first += 8) process_block(first);

    result.codewords = 4;
    for (std::size_t word = 0; word < 4; ++word) {
        const auto fibs = decode_codeword(soft.data() + word * 2304);
        for (const auto& fib : fibs)
            if (valid_fib_crc(fib)) result.fibs.push_back(fib);
    }
    return result;
}

struct PhaseModel {
    double intercept = 0.0;
    double slope = 0.0;
    double coherence = 0.0;
};

struct PhaseCell {
    int carrier = 0;
    std::complex<double> fourth;
    double weight = 0.0;
};

std::complex<double> phase_plane_sum(const std::vector<PhaseCell>& cells,
                                     double slope)
{
    if (cells.empty()) return {};
    auto rotation = std::polar(
        1.0, -4.0 * slope * double(cells.front().carrier));
    const auto step = std::polar(1.0, -4.0 * slope);
    int prior = cells.front().carrier;
    std::complex<double> sum;
    for (const auto& cell : cells) {
        for (int carrier = prior; carrier < cell.carrier; ++carrier)
            rotation *= step;
        sum += cell.weight * cell.fourth * rotation;
        prior = cell.carrier;
    }
    return sum;
}

PhaseModel estimate_phase_model(
    const std::complex<double>* values, const std::array<int, 1536>& carriers,
    bool weighted_phase_plane)
{
    std::vector<double> magnitudes;
    magnitudes.reserve(carriers.size());
    for (std::size_t index = 0; index < carriers.size(); ++index) {
        const auto magnitude = std::abs(values[index]);
        if (magnitude > 1e-12) magnitudes.push_back(magnitude);
    }
    if (magnitudes.empty()) return {};
    auto middle = magnitudes.begin() + magnitudes.size() / 2;
    std::nth_element(magnitudes.begin(), middle, magnitudes.end());
    const double median_magnitude = *middle;
    const double weight_cap = std::max(1e-12, 3.0 * median_magnitude);

    std::vector<PhaseCell> cells;
    cells.reserve(carriers.size());
    double weight_sum = 0.0;
    for (std::size_t index = 0; index < carriers.size(); ++index) {
        const auto magnitude = std::abs(values[index]);
        if (magnitude <= 1e-12) continue;
        const auto normalized = values[index] / magnitude;
        const auto square = normalized * normalized;
        const auto fourth = -(square * square);
        const double limited = std::min(magnitude, weight_cap);
        const double weight = weighted_phase_plane ? limited * limited : 1.0;
        cells.push_back({carriers[index], fourth, weight});
        weight_sum += weight;
    }
    std::sort(cells.begin(), cells.end(), [](const auto& lhs,
                                             const auto& rhs) {
        return lhs.carrier < rhs.carrier;
    });
    if (cells.empty() || weight_sum <= 0.0) return {};

    double best_slope = 0.0;
    auto best_sum = phase_plane_sum(cells, 0.0);
    double best_coherence = std::abs(best_sum) / weight_sum;
    if (weighted_phase_plane) {
        // +/-0.004 rad/carrier spans slightly more than one sample of
        // symbol-to-symbol timing drift. The fourth-power metric removes the
        // unknown QPSK data before the bounded phase-plane search.
        constexpr double radius = 0.004;
        constexpr double coarse_step = 0.000125;
        for (double slope = -radius; slope <= radius + 0.5 * coarse_step;
             slope += coarse_step) {
            const auto sum = phase_plane_sum(cells, slope);
            const double coherence = std::abs(sum) / weight_sum;
            if (coherence > best_coherence) {
                best_coherence = coherence;
                best_slope = slope;
                best_sum = sum;
            }
        }
        constexpr double fine_step = coarse_step / 10.0;
        const double coarse_best = best_slope;
        for (double slope = coarse_best - coarse_step;
             slope <= coarse_best + coarse_step + 0.5 * fine_step;
             slope += fine_step) {
            if (slope < -radius || slope > radius) continue;
            const auto sum = phase_plane_sum(cells, slope);
            const double coherence = std::abs(sum) / weight_sum;
            if (coherence > best_coherence) {
                best_coherence = coherence;
                best_slope = slope;
                best_sum = sum;
            }
        }

        // At very low SNR a slope search can fit random phases. Require a
        // material coherence improvement over the weighted intercept-only
        // estimate before applying it to decoder metrics.
        const auto intercept_only = phase_plane_sum(cells, 0.0);
        const double intercept_coherence =
            std::abs(intercept_only) / weight_sum;
        if (best_coherence < intercept_coherence + 0.01) {
            best_slope = 0.0;
            best_sum = intercept_only;
            best_coherence = intercept_coherence;
        }
    }
    return {0.25 * std::arg(best_sum), best_slope, best_coherence};
}

FrameResult decode_frame_at_window(
    const std::vector<std::complex<float>>& samples, std::size_t null_start,
    double fine_offset_hz, int carrier_offset, bool inverted,
    std::int64_t fft_window_offset, const FicDecodeSettings& settings,
    std::vector<ConstellationPoint>* constellation = nullptr)
{
    if (settings.cancel_requested && settings.cancel_requested()) return {};
    const auto signed_prs_start = std::int64_t(null_start) +
                                  std::int64_t(null_size + guard) +
                                  fft_window_offset;
    if (signed_prs_start < 0) return {};
    const std::size_t prs_start = std::size_t(signed_prs_start);
    if (prs_start + 3 * symbol_size + fft_size > samples.size()) return {};
    std::array<std::array<std::complex<double>, fft_size>, 4> spectra{};
    for (std::size_t symbol = 0; symbol < 4; ++symbol)
        spectra[symbol] = symbol_fft(samples, prs_start + symbol * symbol_size,
                                     fine_offset_hz, carrier_offset, inverted);

    std::array<std::complex<double>, 4608> differential{};
    std::size_t dindex = 0;
    const auto& map = carrier_map();
    for (std::size_t symbol = 1; symbol < 4; ++symbol) {
        for (const int carrier : map) {
            const auto bin =
                std::size_t((carrier + int(fft_size)) % int(fft_size));
            const auto current = spectra[symbol][bin];
            const auto previous = spectra[symbol - 1][bin];
            const auto value = current * std::conj(previous);
            // Preserve carrier reliability for the soft Viterbi metrics.
            // Division by joint RMS removes one power of channel amplitude
            // without promoting deep fades to full-confidence decisions.
            const double joint_rms =
                std::sqrt(std::norm(current) + std::norm(previous));
            differential[dindex++] =
                joint_rms > 1e-12 ? value / joint_rms : std::complex<double>{};
        }
    }
    const auto decode_phase_hypothesis = [&](bool weighted_phase_plane,
                                             std::vector<ConstellationPoint>*
                                                 hypothesis_constellation) {
        FrameResult hypothesis_best;
        std::array<PhaseModel, 3> phase_models{};
        for (std::size_t symbol = 0; symbol < 3; ++symbol) {
            phase_models[symbol] = estimate_phase_model(
                differential.data() + symbol * 1536, map, weighted_phase_plane);
        }

        DqpskQuality quality;
        quality.available = true;
        quality.phase_plane_enabled = weighted_phase_plane;
        quality.phase_plane_attempted = weighted_phase_plane;
        quality.adaptive_phase_plane = settings.adaptive_phase_plane;
        quality.adaptive_fft_window = settings.adaptive_fft_window;
        quality.intercept_frames = weighted_phase_plane ? 0 : 1;
        quality.weighted_frames = weighted_phase_plane ? 1 : 0;
        quality.fft_window_adjusted_frames = fft_window_offset == 0 ? 0 : 1;
        quality.mean_fft_window_offset_samples = double(fft_window_offset);
        quality.symbols = 3;
        double evm_sum = 0.0;
        std::array<double, 3> symbol_rms{};
        for (std::size_t symbol = 0; symbol < 3; ++symbol) {
            const auto& model = phase_models[symbol];
            quality.mean_coherence =
                std::isfinite(quality.mean_coherence)
                    ? quality.mean_coherence + model.coherence
                    : model.coherence;
            quality.mean_phase_intercept_rad =
                std::isfinite(quality.mean_phase_intercept_rad)
                    ? quality.mean_phase_intercept_rad + model.intercept
                    : model.intercept;
            quality.mean_phase_slope_rad_per_carrier =
                std::isfinite(quality.mean_phase_slope_rad_per_carrier)
                    ? quality.mean_phase_slope_rad_per_carrier + model.slope
                    : model.slope;

            double power = 0.0;
            for (std::size_t index = 0; index < 1536; ++index) {
                const auto rotation = std::polar(
                    1.0, -(model.intercept + model.slope * double(map[index])));
                power +=
                    std::norm(differential[symbol * 1536 + index] * rotation);
            }
            const double rms = std::sqrt(power / 1536.0);
            symbol_rms[symbol] = rms;
            double error = 0.0;
            if (rms > 1e-12) {
                constexpr double target = 0.7071067811865475;
                for (std::size_t index = 0; index < 1536; ++index) {
                    const auto rotation =
                        std::polar(1.0, -(model.intercept +
                                          model.slope * double(map[index])));
                    const auto value =
                        differential[symbol * 1536 + index] * rotation / rms;
                    const double ideal_real =
                        value.real() >= 0.0 ? target : -target;
                    const double ideal_imag =
                        value.imag() >= 0.0 ? target : -target;
                    error += std::norm(
                        value - std::complex<double>(ideal_real, ideal_imag));
                }
                evm_sum += std::sqrt(error / 1536.0);
            }
        }
        quality.mean_coherence /= 3.0;
        quality.mean_phase_intercept_rad /= 3.0;
        quality.mean_phase_slope_rad_per_carrier /= 3.0;
        quality.mean_evm_rms = evm_sum / 3.0;
        quality.mean_mer_db = quality.mean_evm_rms > 0.0
                                  ? -20.0 * std::log10(quality.mean_evm_rms)
                                  : std::numeric_limits<double>::infinity();
        hypothesis_best.dqpsk = quality;

        if (hypothesis_constellation) {
            hypothesis_constellation->clear();
            hypothesis_constellation->reserve(4608);
            for (std::size_t symbol = 0; symbol < 3; ++symbol) {
                const auto& model = phase_models[symbol];
                const double rms = symbol_rms[symbol];
                if (rms <= 1e-12) continue;
                for (std::size_t carrier = 0; carrier < 1536; ++carrier) {
                    const auto rotation =
                        std::polar(1.0, -(model.intercept +
                                          model.slope * double(map[carrier])));
                    const auto value =
                        differential[symbol * 1536 + carrier] * rotation / rms;
                    hypothesis_constellation->push_back(
                        {{float(value.real()), float(value.imag())},
                         std::uint8_t(symbol)});
                }
            }
        }

        for (int quadrant = 0; quadrant < 4; ++quadrant) {
            if (settings.cancel_requested && settings.cancel_requested())
                return FrameResult{};
            std::array<float, 9216> soft{};
            for (std::size_t symbol = 0; symbol < 3; ++symbol) {
                const auto& model = phase_models[symbol];
                for (std::size_t carrier = 0; carrier < 1536; ++carrier) {
                    const auto rotation =
                        std::polar(1.0, -(model.intercept +
                                          model.slope * double(map[carrier]) +
                                          quadrant * M_PI / 2.0));
                    const auto value =
                        differential[symbol * 1536 + carrier] * rotation;
                    // EN 300 401 clause 14.5 maps p(l,n) to I and
                    // p(l,n+K) to Q; the two bit planes are not interleaved.
                    soft[symbol * 3072 + carrier] = float(value.real());
                    soft[symbol * 3072 + 1536 + carrier] = float(value.imag());
                }
            }
            FrameResult current;
            current.codewords = 4;
            for (std::size_t word = 0; word < 4; ++word) {
                const auto fibs = decode_codeword(soft.data() + word * 2304);
                for (const auto& fib : fibs)
                    if (valid_fib_crc(fib)) current.fibs.push_back(fib);
            }
            current.dqpsk = quality;
            if (current.fibs.size() > hypothesis_best.fibs.size())
                hypothesis_best = std::move(current);
            else if (hypothesis_best.codewords == 0)
                hypothesis_best.codewords = current.codewords;
        }
        return hypothesis_best;
    };

    if (!settings.adaptive_phase_plane)
        return decode_phase_hypothesis(settings.weighted_phase_plane,
                                       constellation);

    std::vector<ConstellationPoint> intercept_constellation;
    auto intercept = decode_phase_hypothesis(
        false, constellation ? &intercept_constellation : nullptr);
    constexpr std::size_t max_fibs_per_frame = 12;
    if (intercept.fibs.size() == max_fibs_per_frame) {
        if (constellation) *constellation = std::move(intercept_constellation);
        return intercept;
    }
    if (settings.cancel_requested && settings.cancel_requested())
        return FrameResult{};
    std::vector<ConstellationPoint> weighted_constellation;
    auto weighted = decode_phase_hypothesis(
        true, constellation ? &weighted_constellation : nullptr);
    intercept.dqpsk.phase_plane_attempted = true;
    // CRC is the protocol-level ground truth for this bounded hypothesis
    // choice. Never merge correlated FIBs from both paths. The strict
    // comparison preserves the former intercept-only route on a tie.
    if (weighted.fibs.size() > intercept.fibs.size()) {
        if (constellation) *constellation = std::move(weighted_constellation);
        return weighted;
    }
    if (constellation) *constellation = std::move(intercept_constellation);
    return intercept;
}

FrameResult
decode_frame(const std::vector<std::complex<float>>& samples,
             std::size_t null_start, double fine_offset_hz, int carrier_offset,
             bool inverted, const FicDecodeSettings& settings,
             std::vector<ConstellationPoint>* constellation = nullptr)
{
    auto best = decode_frame_at_window(samples, null_start, fine_offset_hz,
                                       carrier_offset, inverted, 0, settings,
                                       constellation);
    if (!settings.adaptive_fft_window) return best;
    constexpr std::size_t max_fibs_per_frame = 12;
    if (best.fibs.size() == max_fibs_per_frame) return best;

    // Negative offsets remain within the 504-sample cyclic prefix and avoid
    // moving the FFT into the following symbol. Search most of the guard
    // interval at 64-sample (31.25 us) spacing. This includes the former
    // measured -192-sample optimum and a -64-sample hypothesis close to the
    // repeatable 59-sample path separation observed on live 10A. CRC is the
    // protocol-level selection metric; offset zero and earlier candidates win
    // ties, and a complete frame stops the search.
    constexpr std::array<std::int64_t, 7> offsets = {
        -64, -128, -192, -256, -320, -384, -448};
    for (const auto offset : offsets) {
        if (settings.cancel_requested && settings.cancel_requested()) break;
        std::vector<ConstellationPoint> candidate_constellation;
        auto candidate = decode_frame_at_window(
            samples, null_start, fine_offset_hz, carrier_offset, inverted,
            offset, settings,
            constellation ? &candidate_constellation : nullptr);
        // Offset zero deliberately wins ties.
        if (candidate.fibs.size() <= best.fibs.size()) continue;
        best = std::move(candidate);
        if (constellation) *constellation = std::move(candidate_constellation);
        if (best.fibs.size() == max_fibs_per_frame) break;
    }
    return best;
}

struct PhaseCandidate {
    std::size_t phase = 0;
    int carrier_offset = 0;
    bool inverted = false;
};

struct QualityAccumulator {
    bool phase_plane_enabled = false;
    bool phase_plane_attempted = false;
    bool adaptive_phase_plane = false;
    bool adaptive_fft_window = false;
    std::size_t frames = 0;
    std::size_t intercept_frames = 0;
    std::size_t weighted_frames = 0;
    std::size_t fft_window_adjusted_frames = 0;
    double fft_window_offset_sum = 0.0;
    double coherence = 0.0;
    double evm = 0.0;
    double mer = 0.0;
    double intercept = 0.0;
    double slope = 0.0;

    void add(const DqpskQuality& value)
    {
        if (!value.available) return;
        phase_plane_enabled = phase_plane_enabled || value.phase_plane_enabled;
        phase_plane_attempted =
            phase_plane_attempted || value.phase_plane_attempted;
        adaptive_phase_plane = adaptive_phase_plane ||
            value.adaptive_phase_plane;
        adaptive_fft_window = adaptive_fft_window ||
            value.adaptive_fft_window;
        ++frames;
        intercept_frames += value.intercept_frames;
        weighted_frames += value.weighted_frames;
        fft_window_adjusted_frames += value.fft_window_adjusted_frames;
        fft_window_offset_sum += value.mean_fft_window_offset_samples;
        coherence += value.mean_coherence;
        evm += value.mean_evm_rms;
        mer += value.mean_mer_db;
        intercept += value.mean_phase_intercept_rad;
        slope += value.mean_phase_slope_rad_per_carrier;
    }

    DqpskQuality result() const
    {
        DqpskQuality value;
        if (frames == 0) return value;
        value.available = true;
        value.phase_plane_enabled = phase_plane_enabled;
        value.phase_plane_attempted = phase_plane_attempted;
        value.adaptive_phase_plane = adaptive_phase_plane;
        value.adaptive_fft_window = adaptive_fft_window;
        value.intercept_frames = intercept_frames;
        value.weighted_frames = weighted_frames;
        value.fft_window_adjusted_frames = fft_window_adjusted_frames;
        value.mean_fft_window_offset_samples =
            fft_window_offset_sum / frames;
        value.symbols = 3 * frames;
        value.mean_coherence = coherence / frames;
        value.mean_evm_rms = evm / frames;
        value.mean_mer_db = mer / frames;
        value.mean_phase_intercept_rad = intercept / frames;
        value.mean_phase_slope_rad_per_carrier = slope / frames;
        return value;
    }
};

std::optional<PhaseCandidate> strongest_phase_candidate(
    const std::vector<std::complex<float>>& samples, const Detection& detection,
    const FicDecodeSettings& settings = {})
{
    if (!detection.null_sync) return std::nullopt;
    TimingSearchConfig timing{
        sample_rate, 2500.0, 16, 2, 6, 504, 504, 2,
        0.05, 0.20, 10, false};
    timing.cancel_requested = settings.cancel_requested;
    const auto candidates = find_mode_i_timing_candidates(
        samples, detection.null_start_sample, timing);
    for (const auto& value : candidates) {
        if (!value.prs_confirmed) continue;
        return PhaseCandidate{refined_frame_phase_sample(value),
                              value.prs_integer_carrier_offset,
                              value.prs_spectrum_inverted};
    }
    return PhaseCandidate{detection.null_start_sample % frame_size, 0, false};
}

DecodeResult decode_candidates(
    const std::vector<std::complex<float>>& samples,
    const std::vector<PhaseCandidate>& candidates, std::size_t max_frames,
    double fine_offset_hz, const FicDecodeSettings& settings,
    bool rank_temporally = false)
{
    DecodeResult result;
    result.stats.timing_candidates = candidates.size();
    IdentityAccumulator identity;
    QualityAccumulator quality;
    for (const auto& candidate : candidates) {
        if (settings.cancel_requested && settings.cancel_requested()) break;
        const auto phase = candidate.phase % frame_size;
        const auto available = phase + frame_size <= samples.size()
            ? 1 + (samples.size() - phase - frame_size) / frame_size : 0;
        result.stats.frames_available += available;
        std::vector<ModeIFrameQuality> selected;
        if (rank_temporally) {
            selected = rank_mode_i_frames(
                samples, phase, max_frames, settings.cancel_requested);
            result.stats.temporal_ranked = true;
            if (!selected.empty()) {
                result.stats.best_frame_cp_metric = selected.front().cp_metric;
                result.stats.best_frame_null_depth_db =
                    selected.front().null_depth_db;
                const auto by_time = std::minmax_element(
                    selected.begin(), selected.end(), [](const auto& lhs,
                                                          const auto& rhs) {
                        return lhs.frame_start_sample < rhs.frame_start_sample;
                    });
                result.stats.selected_first_sample =
                    by_time.first->frame_start_sample;
                result.stats.selected_last_sample =
                    by_time.second->frame_start_sample;
            }
        } else {
            for (std::size_t start = phase;
                 start + frame_size <= samples.size() &&
                     selected.size() < max_frames;
                 start += frame_size)
                selected.push_back({start, 0.0, 0.0, 0.0});
        }
        for (const auto& selected_frame : selected) {
            if (settings.cancel_requested && settings.cancel_requested()) break;
            const auto start = selected_frame.frame_start_sample;
            auto frame = decode_frame(samples, start, fine_offset_hz,
                                      candidate.carrier_offset,
                                      candidate.inverted, settings);
            ++result.stats.frames_examined;
            result.stats.codewords_decoded += frame.codewords;
            result.stats.valid_fibs += frame.fibs.size();
            quality.add(frame.dqpsk);
            result.stats.dqpsk = quality.result();
            for (const auto& fib : frame.fibs) {
                identity.fic.available = true;
                ++identity.fic.valid_fibs;
                parse_fib(fib, identity);
            }
            update_decode_identity(result, identity);
            // Preserve the former fast path when the complete human-readable
            // identity is available. Without a label, continue consuming this
            // capture but retain ECC+EId as a valid result at end-of-input.
            if (result.ensemble.decoded && !result.ensemble.label.empty())
                return result;
        }
    }
    update_decode_identity(result, identity);
    return result;
}

DecodeResult decode_candidates_two_dimensional(
    const std::vector<std::complex<float>>& samples,
    const std::vector<PhaseCandidate>& candidates, std::size_t max_frames,
    double fine_offset_hz, const FicDecodeSettings& settings,
    bool rank_temporally)
{
    const auto started = std::chrono::steady_clock::now();
    DecodeResult result;
    result.stats.timing_candidates = candidates.size();
    result.stats.demodulation.two_dimensional_attempted = true;
    IdentityAccumulator identity;
    for (const auto& candidate : candidates) {
        if (settings.cancel_requested && settings.cancel_requested()) break;
        const auto phase = candidate.phase % frame_size;
        const auto available = phase + frame_size <= samples.size()
            ? 1 + (samples.size() - phase - frame_size) / frame_size : 0;
        result.stats.frames_available += available;
        std::vector<ModeIFrameQuality> selected;
        if (rank_temporally) {
            selected = rank_mode_i_frames(
                samples, phase, max_frames, settings.cancel_requested);
            result.stats.temporal_ranked = true;
            if (!selected.empty()) {
                result.stats.best_frame_cp_metric = selected.front().cp_metric;
                result.stats.best_frame_null_depth_db =
                    selected.front().null_depth_db;
                const auto by_time = std::minmax_element(
                    selected.begin(), selected.end(), [](const auto& lhs,
                                                          const auto& rhs) {
                        return lhs.frame_start_sample < rhs.frame_start_sample;
                    });
                result.stats.selected_first_sample =
                    by_time.first->frame_start_sample;
                result.stats.selected_last_sample =
                    by_time.second->frame_start_sample;
            }
        } else {
            for (std::size_t start = phase;
                 start + frame_size <= samples.size() &&
                     selected.size() < max_frames;
                 start += frame_size)
                selected.push_back({start, 0.0, 0.0, 0.0});
        }

        for (const auto& selected_frame : selected) {
            if (settings.cancel_requested && settings.cancel_requested()) break;
            const auto frame = decode_frame_two_dimensional(
                samples, selected_frame.frame_start_sample, fine_offset_hz,
                candidate.carrier_offset, candidate.inverted);
            ++result.stats.frames_examined;
            result.stats.codewords_decoded += frame.codewords;
            result.stats.valid_fibs += frame.fibs.size();
            for (const auto& fib : frame.fibs) {
                identity.fic.available = true;
                ++identity.fic.valid_fibs;
                parse_fib(fib, identity);
            }
            update_decode_identity(result, identity);
            if (result.ensemble.decoded && !result.ensemble.label.empty())
                break;
        }
        if (result.ensemble.decoded && !result.ensemble.label.empty()) break;
    }
    update_decode_identity(result, identity);
    result.stats.demodulation.selected_route = "TWO_DIMENSIONAL_8X4";
    result.stats.demodulation.two_dimensional_succeeded =
        result.ensemble.decoded;
    result.stats.demodulation.two_dimensional_frames_examined =
        result.stats.frames_examined;
    result.stats.demodulation.two_dimensional_codewords_decoded =
        result.stats.codewords_decoded;
    result.stats.demodulation.two_dimensional_valid_fibs =
        result.stats.valid_fibs;
    result.stats.demodulation.two_dimensional_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
    result.ensemble.demodulation = result.stats.demodulation;
    return result;
}

DecodeResult apply_two_dimensional_fallback(
    DecodeResult primary, const std::vector<std::complex<float>>& samples,
    const std::vector<PhaseCandidate>& candidates, std::size_t max_frames,
    double fine_offset_hz, const FicDecodeSettings& settings,
    bool rank_temporally)
{
    if (primary.ensemble.decoded || !settings.two_dimensional_fallback ||
        (settings.cancel_requested && settings.cancel_requested())) {
        primary.ensemble.demodulation = primary.stats.demodulation;
        return primary;
    }

    auto fallback = decode_candidates_two_dimensional(
        samples, candidates, max_frames, fine_offset_hz, settings,
        rank_temporally);
    auto attempt = fallback.stats.demodulation;
    if (fallback.ensemble.decoded) {
        fallback.stats.demodulation = attempt;
        fallback.ensemble.demodulation = attempt;
        return fallback;
    }
    primary.stats.demodulation.two_dimensional_attempted = true;
    primary.stats.demodulation.two_dimensional_succeeded = false;
    primary.stats.demodulation.two_dimensional_frames_examined =
        attempt.two_dimensional_frames_examined;
    primary.stats.demodulation.two_dimensional_codewords_decoded =
        attempt.two_dimensional_codewords_decoded;
    primary.stats.demodulation.two_dimensional_valid_fibs =
        attempt.two_dimensional_valid_fibs;
    primary.stats.demodulation.two_dimensional_ms =
        attempt.two_dimensional_ms;
    primary.ensemble.demodulation = primary.stats.demodulation;
    return primary;
}

} // namespace

DecodeResult decode_fic(const std::vector<std::complex<float>>& samples,
                        std::uint32_t, const FicDecodeSettings& settings)
{
    if (settings.cancel_requested && settings.cancel_requested()) return {};
    if (samples.size() < frame_size) return {};
    const auto detection = detect_mode_i(
        samples, {sample_rate, 0.16, 20, 1.0});
    if (!detection.null_sync) return {};
    std::vector<PhaseCandidate> candidates;
    if (const auto candidate = strongest_phase_candidate(
            samples, detection, settings))
        candidates.push_back(*candidate);
    // The empty-channel scheduler captures in bounded FIC-sized blocks. A
    // fixed ten-frame limit examined only about 0.96 s of a bounded scanner
    // block and made a weak-signal comparison with libdabsdr unfair. Strong
    // muxes still return as soon as their CRC-valid identity and label are
    // complete; otherwise consume every complete frame in this block.
    const std::size_t available_frames = samples.size() / frame_size + 1;
    auto result = settings.two_dimensional_only
        ? decode_candidates_two_dimensional(
              samples, candidates, available_frames, detection.fine_offset_hz,
              settings, false)
        : decode_candidates(samples, candidates, available_frames,
                            detection.fine_offset_hz, settings, false);
    if (settings.cancel_requested && settings.cancel_requested()) return result;
    if (!settings.two_dimensional_only && settings.weighted_phase_plane &&
        !result.ensemble.decoded) {
        FicDecodeSettings legacy_settings;
        legacy_settings.weighted_phase_plane = false;
        legacy_settings.cancel_requested = settings.cancel_requested;
        const auto legacy = decode_candidates(
            samples, candidates, available_frames, detection.fine_offset_hz,
            legacy_settings, false);
        if (legacy.ensemble.decoded) {
            result = legacy;
            result.stats.dqpsk.phase_plane_attempted = true;
            result.stats.dqpsk.legacy_fallback_used = true;
            result.ensemble.dqpsk = result.stats.dqpsk;
        }
    }
    if (!settings.two_dimensional_only)
        result = apply_two_dimensional_fallback(
            std::move(result), samples, candidates, available_frames,
            detection.fine_offset_hz, settings, false);
    result.ensemble.demodulation = result.stats.demodulation;
    return result;
}

DecodeResult decode_fic_at_frame_phase(
    const std::vector<std::complex<float>>& samples, std::uint32_t,
    std::size_t frame_phase_sample, std::size_t max_frames,
    const FicDecodeSettings& settings)
{
    return decode_fic_at_frame_phase(
        samples, 0, frame_phase_sample, max_frames, 0, false, settings);
}

DecodeResult decode_fic_at_frame_phase(
    const std::vector<std::complex<float>>& samples, std::uint32_t,
    std::size_t frame_phase_sample, std::size_t max_frames,
    int integer_carrier_offset, bool spectrum_inverted,
    const FicDecodeSettings& settings)
{
    if (max_frames == 0 || samples.size() < frame_size) return {};
    if (settings.cancel_requested && settings.cancel_requested()) return {};
    const auto detection = detect_mode_i(
        samples, {sample_rate, 0.16, 1, 0.0});
    const std::vector<PhaseCandidate> candidates{{
        frame_phase_sample % frame_size, integer_carrier_offset,
        spectrum_inverted}};
    auto result = settings.two_dimensional_only
        ? decode_candidates_two_dimensional(
              samples, candidates, max_frames, detection.fine_offset_hz,
              settings, true)
        : decode_candidates(samples, candidates, max_frames,
                            detection.fine_offset_hz, settings, true);
    if (!settings.two_dimensional_only && settings.weighted_phase_plane &&
        !result.ensemble.decoded) {
        FicDecodeSettings legacy_settings;
        legacy_settings.weighted_phase_plane = false;
        legacy_settings.cancel_requested = settings.cancel_requested;
        const auto legacy = decode_candidates(
            samples, {{frame_phase_sample % frame_size,
                       integer_carrier_offset, spectrum_inverted}},
            max_frames, detection.fine_offset_hz, legacy_settings, true);
        if (legacy.ensemble.decoded) {
            result = legacy;
            result.stats.dqpsk.phase_plane_attempted = true;
            result.stats.dqpsk.legacy_fallback_used = true;
            result.ensemble.dqpsk = result.stats.dqpsk;
        }
    }
    if (!settings.two_dimensional_only)
        result = apply_two_dimensional_fallback(
            std::move(result), samples, candidates, max_frames,
            detection.fine_offset_hz, settings, true);
    result.ensemble.demodulation = result.stats.demodulation;
    return result;
}

ConstellationSnapshot extract_constellation(
    const std::vector<std::complex<float>>& samples, std::size_t max_points,
    const FicDecodeSettings& settings)
{
    if (max_points == 0 || samples.size() < frame_size) return {};
    const auto detection = detect_mode_i(
        samples, {sample_rate, 0.16, 20, 1.0});
    const auto candidate = strongest_phase_candidate(samples, detection, settings);
    if (!candidate) return {};
    return extract_constellation_at_frame_phase(
        samples, candidate->phase, detection.fine_offset_hz,
        candidate->carrier_offset, candidate->inverted,
        max_points, settings);
}

ConstellationSnapshot extract_constellation_at_frame_phase(
    const std::vector<std::complex<float>>& samples,
    std::size_t frame_phase_sample, double fine_offset_hz,
    int integer_carrier_offset, bool spectrum_inverted,
    std::size_t max_points, const FicDecodeSettings& settings)
{
    ConstellationSnapshot result;
    if (max_points == 0 || samples.size() < frame_size) return result;
    std::size_t frame_start = frame_phase_sample % frame_size;
    while (frame_start + 2 * frame_size <= samples.size())
        frame_start += frame_size;

    const std::size_t prs_start = frame_start + null_size + guard;
    constexpr std::size_t data_symbol_count = 75;
    if (prs_start + data_symbol_count * symbol_size + fft_size > samples.size())
        return result;

    std::vector<ConstellationPoint> all_points;
    all_points.reserve(data_symbol_count * 1536);
    const auto& map = carrier_map();
    auto previous = symbol_fft(
        samples, prs_start, fine_offset_hz,
        integer_carrier_offset, spectrum_inverted);
    double coherence_sum = 0.0;
    double evm_sum = 0.0;
    double intercept_sum = 0.0;
    double slope_sum = 0.0;
    for (std::size_t symbol = 0; symbol < data_symbol_count; ++symbol) {
        auto current = symbol_fft(
            samples, prs_start + (symbol + 1) * symbol_size,
            fine_offset_hz, integer_carrier_offset, spectrum_inverted);
        std::array<std::complex<double>, 1536> differential{};
        for (std::size_t index = 0; index < map.size(); ++index) {
            const auto bin = std::size_t(
                (map[index] + int(fft_size)) % int(fft_size));
            const auto value = current[bin] * std::conj(previous[bin]);
            const double joint_rms = std::sqrt(
                std::norm(current[bin]) + std::norm(previous[bin]));
            differential[index] = joint_rms > 1e-12
                ? value / joint_rms : std::complex<double>{};
        }
        const auto model = estimate_phase_model(
            differential.data(), map, settings.weighted_phase_plane);
        coherence_sum += model.coherence;
        intercept_sum += model.intercept;
        slope_sum += model.slope;

        double power = 0.0;
        for (std::size_t index = 0; index < map.size(); ++index) {
            const auto rotation = std::polar(
                1.0, -(model.intercept + model.slope * double(map[index])));
            power += std::norm(differential[index] * rotation);
        }
        const double rms = std::sqrt(power / double(map.size()));
        double error = 0.0;
        if (rms > 1e-12) {
            constexpr double target = 0.7071067811865475;
            for (std::size_t index = 0; index < map.size(); ++index) {
                const auto rotation = std::polar(
                    1.0, -(model.intercept +
                           model.slope * double(map[index])));
                const auto value = differential[index] * rotation / rms;
                const double ideal_real = value.real() >= 0.0
                    ? target : -target;
                const double ideal_imag = value.imag() >= 0.0
                    ? target : -target;
                error += std::norm(
                    value - std::complex<double>(ideal_real, ideal_imag));
                all_points.push_back({
                    {float(value.real()), float(value.imag())},
                    std::uint8_t(symbol)});
            }
            evm_sum += std::sqrt(error / double(map.size()));
        }
        previous = std::move(current);
    }
    if (all_points.empty()) return result;

    result.data_symbols = data_symbol_count;
    result.quality.available = true;
    result.quality.phase_plane_enabled = settings.weighted_phase_plane;
    result.quality.phase_plane_attempted = settings.weighted_phase_plane;
    result.quality.symbols = data_symbol_count;
    result.quality.mean_coherence = coherence_sum / data_symbol_count;
    result.quality.mean_evm_rms = evm_sum / data_symbol_count;
    result.quality.mean_mer_db = result.quality.mean_evm_rms > 0.0
        ? -20.0 * std::log10(result.quality.mean_evm_rms)
        : std::numeric_limits<double>::infinity();
    result.quality.mean_phase_intercept_rad =
        intercept_sum / data_symbol_count;
    result.quality.mean_phase_slope_rad_per_carrier =
        slope_sum / data_symbol_count;
    result.frame_start_sample = frame_start;
    result.fine_offset_hz = fine_offset_hz;
    result.integer_carrier_offset = integer_carrier_offset;
    result.spectrum_inverted = spectrum_inverted;
    if (all_points.size() <= max_points) {
        result.points = std::move(all_points);
    } else {
        result.points.reserve(max_points);
        for (std::size_t index = 0; index < max_points; ++index)
            result.points.push_back(
                all_points[index * all_points.size() / max_points]);
    }

    std::array<std::vector<double>, 3> magnitudes;
    for (const auto& point : result.points) {
        const auto symbol = std::min<std::size_t>(point.data_symbol / 25, 2);
        const double magnitude = std::abs(point.value);
        if (std::isfinite(magnitude) && magnitude > 1e-12)
            magnitudes[symbol].push_back(magnitude);
    }
    std::array<double, 3> upper_quartile{};
    for (std::size_t symbol = 0; symbol < magnitudes.size(); ++symbol) {
        auto& values = magnitudes[symbol];
        if (values.empty()) continue;
        auto threshold = values.begin() + 3 * values.size() / 4;
        std::nth_element(values.begin(), threshold, values.end());
        upper_quartile[symbol] = *threshold;
    }
    std::complex<double> fourth_sum{};
    for (const auto& point : result.points) {
        const auto symbol = std::min<std::size_t>(point.data_symbol / 25, 2);
        const auto value = std::complex<double>(point.value);
        const double magnitude = std::abs(value);
        if (!std::isfinite(magnitude) || magnitude <= 1e-12 ||
            magnitude < upper_quartile[symbol])
            continue;
        const auto normalized = value / magnitude;
        const auto square = normalized * normalized;
        fourth_sum -= square * square;
        ++result.strong_point_count;
    }
    if (result.strong_point_count)
        result.strong_phase_coherence =
            std::abs(fourth_sum) / double(result.strong_point_count);
    return result;
}

TiiInfo decode_tii_single(const std::vector<std::complex<float>>& samples,
                          const TiiDecodeSettings& settings)
{
    TiiInfo result;
    result.attempted = true;
    result.conservative = settings.conservative;
    result.dwell_ms = 1000.0 * samples.size() / sample_rate;
    if (samples.size() < 2 * frame_size) return result;

    const auto detection = detect_mode_i(
        samples, {sample_rate, 0.16, 20, 1.0});
    const auto candidate = strongest_phase_candidate(samples, detection);
    if (!candidate) return result;

    struct NullMeasurement {
        // Some deployed transmitters follow the current ETSI TII carrier
        // phases while others use the older equal-pair convention. Keep both
        // hypotheses. In either convention, a real transmitter has coherent
        // phase across its four active groups; magnitude-only fitting can
        // turn overlapping patterns into a false Main ID.
        using Cells = std::array<std::array<std::complex<double>, 8>, 24>;
        std::array<Cells, 2> cells{}; // raw pair phase, PRS-phase removed
        std::array<double, 2> total{};
    };
    const auto& prs_reference = prs_reference_spectrum();
    std::vector<NullMeasurement> measurements;
    for (std::size_t start = candidate->phase;
         start + null_size + guard + fft_size <= samples.size();
         start += frame_size) {
        const auto null_spectrum = symbol_fft(
            samples, start + null_size - fft_size,
            detection.fine_offset_hz, candidate->carrier_offset,
            candidate->inverted);
        const auto prs_spectrum = symbol_fft(
            samples, start + null_size + guard,
            detection.fine_offset_hz, candidate->carrier_offset,
            candidate->inverted);
        NullMeasurement measurement;
        constexpr std::array<int, 4> block_starts = {-768, -384, 1, 385};
        for (int comb = 0; comb < 24; ++comb) {
            for (int group = 0; group < 8; ++group) {
                std::array<std::complex<double>, 2> correlation{};
                double reference = 0.0;
                for (const int block : block_starts) {
                    const int carrier = block + 2 * comb + 48 * group;
                    const auto first = std::size_t(
                        (carrier + int(fft_size)) % int(fft_size));
                    const auto second = std::size_t(
                        (carrier + 1 + int(fft_size)) % int(fft_size));
                    const auto pair = null_spectrum[first] *
                        std::conj(null_spectrum[second]);
                    correlation[0] += pair;
                    const auto reference_pair = prs_reference[first] *
                        std::conj(prs_reference[second]);
                    const double reference_pair_level = std::abs(reference_pair);
                    correlation[1] += reference_pair_level > 1e-12
                        ? pair * std::conj(reference_pair) /
                            reference_pair_level
                        : pair;
                    reference += std::sqrt(
                        std::norm(prs_spectrum[first]) *
                        std::norm(prs_spectrum[second]));
                }
                for (std::size_t convention = 0; convention < 2;
                     ++convention) {
                    const std::complex<double> value = reference > 1e-12
                        ? correlation[convention] / reference
                        : std::complex<double>{};
                    measurement.cells[convention][comb][group] = value;
                    measurement.total[convention] += std::abs(value);
                }
            }
        }
        measurements.push_back(std::move(measurement));
    }
    if (measurements.size() < 2) return result;

    // TII is active in every other Mode-I frame. Select the observed parity
    // with the greater median normalized pair energy; absolute CIF parity is
    // not needed for identifier recovery.
    auto parity_median = [&](std::size_t parity) {
        std::vector<double> totals;
        for (std::size_t index = parity; index < measurements.size(); index += 2)
            // Raw pair energy is the stable active/dormant discriminator.
            // PRS-phase removal can also align random dormant-frame cells and
            // must not be allowed to flip the selected CIF parity.
            totals.push_back(measurements[index].total[0]);
        if (totals.empty()) return 0.0;
        auto middle = totals.begin() + totals.size() / 2;
        std::nth_element(totals.begin(), middle, totals.end());
        return *middle;
    };
    const std::size_t active_parity = parity_median(1) > parity_median(0) ? 1 : 0;

    // QIRX evaluates an accumulated TII carrier spectrum rather than forcing
    // an exact occupancy decision on one noisy null symbol. Keep decisions
    // independent enough for DX work by averaging pairs of active symbols
    // (about 384 ms in Mode I).
    std::vector<NullMeasurement> decisions;
    constexpr std::size_t decision_frames = 2;
    for (std::size_t first = active_parity;
         first + 2 * (decision_frames - 1) < measurements.size();
         first += 2 * decision_frames) {
        NullMeasurement decision;
        for (std::size_t part = 0; part < decision_frames; ++part) {
            const auto& source = measurements[first + 2 * part];
            for (std::size_t convention = 0; convention < 2; ++convention) {
                for (std::size_t comb = 0; comb < 24; ++comb) {
                    for (std::size_t group = 0; group < 8; ++group) {
                        decision.cells[convention][comb][group] +=
                            source.cells[convention][comb][group] /
                            double(decision_frames);
                    }
                }
            }
        }
        decisions.push_back(std::move(decision));
    }
    if (decisions.empty()) return result;

    std::array<std::uint8_t, 70> patterns{};
    std::size_t pattern_count = 0;
    for (unsigned mask = 0; mask < 256; ++mask) {
        if (__builtin_popcount(mask) == 4)
            patterns[pattern_count++] = std::uint8_t(mask);
    }
    struct Aggregate {
        std::size_t hits = 0;
        double level_sum = 0.0;
    };
    std::map<std::pair<std::uint8_t, std::uint8_t>, Aggregate> aggregate;
    std::map<std::pair<std::uint8_t, std::uint8_t>, std::size_t>
        collision_aggregate;
    for (std::size_t frame = 0; frame < decisions.size(); ++frame) {
        ++result.notifications;
        std::array<double, 2> global_floor{};
        for (std::size_t convention = 0; convention < 2; ++convention) {
            std::array<double, 192> all_cells{};
            std::size_t cell_index = 0;
            for (const auto& comb : decisions[frame].cells[convention])
                for (const auto value : comb)
                    all_cells[cell_index++] = std::abs(value);
            auto middle = all_cells.begin() + all_cells.size() / 2;
            std::nth_element(all_cells.begin(), middle, all_cells.end());
            global_floor[convention] = std::max(1e-12, *middle);
        }

        for (std::size_t comb = 0; comb < 24; ++comb) {
            struct Hypothesis {
                double quality = -1.0;
                double runner_up = -1.0;
                double coherence = 0.0;
                double active = 0.0;
                double level = 0.0;
                std::size_t pattern = 0;
                std::size_t convention = 0;
                std::uint8_t active_mask = 0;
                std::size_t active_blocks = 0;
            };
            std::array<Hypothesis, 2> hypothesis{};
            for (std::size_t convention = 0; convention < 2;
                 ++convention) {
                auto& selected = hypothesis[convention];
                selected.convention = convention;
                double strongest_cell = 0.0;
                for (std::size_t group = 0; group < 8; ++group)
                    strongest_cell = std::max(strongest_cell, std::abs(
                        decisions[frame].cells[convention][comb][group]));
                const double occupied_threshold = std::max(
                    settings.collision_noise_level_ratio *
                        global_floor[convention],
                    settings.collision_relative_threshold * strongest_cell);
                for (std::size_t group = 0; group < 8; ++group) {
                    const auto cell = decisions[frame]
                        .cells[convention][comb][group];
                    if (std::abs(cell) >= occupied_threshold) {
                        selected.active_mask |= std::uint8_t(
                            1U << (7 - group));
                        ++selected.active_blocks;
                    }
                }
                for (std::size_t pattern = 0; pattern < patterns.size();
                     ++pattern) {
                    std::complex<double> active_sum{};
                    double active_magnitude = 0.0, inactive = 0.0;
                    for (std::size_t group = 0; group < 8; ++group) {
                        const auto cell =
                            decisions[frame].cells[convention][comb][group];
                        if (patterns[pattern] & (1U << (7 - group))) {
                            active_sum += cell;
                            active_magnitude += std::abs(cell);
                        }
                        else
                            inactive += std::abs(cell);
                    }
                    const double active = std::abs(active_sum) / 4.0;
                    const double level = active_magnitude / 4.0;
                    inactive /= 4.0;
                    const double contrast = (active - inactive) /
                        (active + inactive + 1e-12);
                    const double coherence = std::abs(active_sum) /
                        (active_magnitude + 1e-12);
                    const double quality = contrast * coherence;
                    if (quality > selected.quality) {
                        selected.runner_up = selected.quality;
                        selected.quality = quality;
                        selected.coherence = coherence;
                        selected.active = active;
                        selected.level = level;
                        selected.pattern = pattern;
                    } else if (quality > selected.runner_up) {
                        selected.runner_up = quality;
                    }
                }
            }

            // Select a convention only after it independently identifies an
            // unambiguous pattern. Comparing all 140 pattern/convention pairs
            // together lets a transform of the same physical signal consume
            // the runner-up margin. Quality times margin favours the phase
            // convention with the clearest Main-ID decision.
            const Hypothesis* best = nullptr;
            double best_score = -1.0;
            for (const auto& candidate : hypothesis) {
                const double margin = candidate.quality - candidate.runner_up;
                // QIRX' collision rule: five-or-more occupied groups for one
                // SubId are ambiguous. Four must exactly match the selected
                // pattern. An under-threshold group may still be recovered by
                // coherent pattern correlation, but every occupied group must
                // belong to that pattern.
                const auto pattern_mask = patterns[candidate.pattern];
                const bool occupancy_consistent =
                    candidate.active_blocks < 4
                        ? (candidate.active_mask & ~pattern_mask) == 0
                        : (candidate.active_blocks == 4 &&
                           pattern_mask == candidate.active_mask);
                if (!occupancy_consistent ||
                    candidate.quality < settings.min_pattern_quality ||
                    candidate.coherence < settings.min_phase_coherence ||
                    margin < settings.min_pattern_margin ||
                    candidate.active < settings.min_cell_level_ratio *
                        global_floor[candidate.convention])
                    continue;
                const double score = candidate.quality * margin;
                if (score > best_score) {
                    best = &candidate;
                    best_score = score;
                }
            }
            if (!best) {
                const Hypothesis* collision = nullptr;
                for (const auto& candidate : hypothesis) {
                    if (candidate.active_blocks <= 4) continue;
                    if (!collision || candidate.active_blocks <
                            collision->active_blocks)
                        collision = &candidate;
                }
                if (collision) {
                    ++collision_aggregate[{std::uint8_t(comb),
                                           collision->active_mask}];
                }
                continue;
            }
            auto& found = aggregate[{std::uint8_t(best->pattern),
                                     std::uint8_t(comb)}];
            ++found.hits;
            found.level_sum += best->level;
        }
    }

    double strongest = 0.0;
    for (const auto& [code, value] : aggregate) {
        (void)code;
        if (value.hits)
            strongest = std::max(strongest, value.level_sum / value.hits);
    }
    for (const auto& [code, value] : aggregate) {
        if (!value.hits) continue;
        TiiCandidate decoded;
        decoded.main_id = code.first;
        decoded.sub_id = code.second;
        const double average = value.level_sum / value.hits;
        // The carrier-pair metric is proportional to signal power.  Report a
        // voltage/amplitude ratio, matching the meaning of the monitor's
        // existing relative TII level rather than a power ratio.
        decoded.level = strongest > 0.0
            ? std::sqrt(average / strongest) : 0.0;
        decoded.hits = value.hits;
        decoded.confidence = result.notifications
            ? double(value.hits) / result.notifications : 0.0;
        // A single strong TII produces low-level spectral neighbours and
        // random four-of-eight fits. In conservative mode, require temporal
        // persistence as well as the caller's absolute hit count before a
        // code may enter heard.tsv. Default mode deliberately accepts shorter
        // propagation enhancements, while still rejecting one-off fits.
        decoded.confirmed = value.hits >= settings.min_hits &&
            decoded.confidence >= settings.min_confirmed_confidence;
        result.candidates.push_back(decoded);
    }

    for (const auto& [key, hits] : collision_aggregate) {
        if (hits < settings.min_hits) continue;
        TiiCollision collision;
        collision.sub_id = key.first;
        collision.active_mask = key.second;
        collision.active_blocks = std::uint8_t(
            __builtin_popcount(unsigned(collision.active_mask)));
        collision.hits = hits;
        collision.confidence = result.notifications
            ? double(hits) / result.notifications : 0.0;
        for (std::size_t main = 0; main < patterns.size(); ++main) {
            if ((patterns[main] & collision.active_mask) == patterns[main])
                collision.possible_main_ids.push_back(std::uint8_t(main));
        }
        result.collisions.push_back(std::move(collision));
    }
    std::sort(result.collisions.begin(), result.collisions.end(),
        [](const auto& lhs, const auto& rhs) {
            if (lhs.hits != rhs.hits) return lhs.hits > rhs.hits;
            if (lhs.sub_id != rhs.sub_id) return lhs.sub_id < rhs.sub_id;
            return lhs.active_mask < rhs.active_mask;
        });

    // A dominant comb can leak into its immediate frequency neighbours when
    // timing, filtering or a truncated null window is imperfect. In
    // conservative mode retain such a result as a candidate, but do not log
    // it as a transmitter when it is below 3% of a persistent adjacent comb.
    if (settings.adjacent_max_relative_level > 0.0 &&
        settings.adjacent_min_dominance > 0.0) {
        for (auto& candidate : result.candidates) {
            if (!candidate.confirmed ||
                candidate.level >= settings.adjacent_max_relative_level)
                continue;
            const bool dominated_by_neighbour = std::any_of(
                result.candidates.begin(), result.candidates.end(),
                [&](const auto& neighbour) {
                    if (&candidate == &neighbour || !neighbour.confirmed)
                        return false;
                    const int distance = std::abs(
                        int(candidate.sub_id) - int(neighbour.sub_id));
                    const bool adjacent = distance == 1 || distance == 23;
                    return adjacent && neighbour.level >=
                        settings.adjacent_min_dominance * candidate.level;
                });
            if (dominated_by_neighbour) candidate.confirmed = false;
        }
    }

    // Conservative JSON output is diagnostic, but thousands of one-frame
    // four-of-eight fits obscure real weak candidates. Keep candidates that
    // recur in at least 10% of active TII frames (and never less than the
    // configured absolute hit requirement). Default mode remains permissive.
    const std::size_t candidate_hits = std::max(
        settings.min_hits, std::size_t(std::ceil(
            settings.candidate_min_confidence * result.notifications)));
    result.candidates.erase(std::remove_if(
        result.candidates.begin(), result.candidates.end(),
        [&](const auto& candidate) { return candidate.hits < candidate_hits; }),
        result.candidates.end());
    std::sort(result.candidates.begin(), result.candidates.end(),
        [](const auto& lhs, const auto& rhs) {
            if (lhs.confirmed != rhs.confirmed) return lhs.confirmed > rhs.confirmed;
            if (lhs.hits != rhs.hits) return lhs.hits > rhs.hits;
            return lhs.level > rhs.level;
        });
    return result;
}

TiiInfo decode_tii(const std::vector<std::complex<float>>& samples,
                   std::uint32_t, const TiiDecodeSettings& settings)
{
    const auto target_samples = std::max<std::size_t>(
        2 * frame_size, std::size_t(std::llround(
            settings.subdwell_ms * sample_rate / 1000.0)));
    std::size_t subdwell_count = target_samples
        ? samples.size() / target_samples : 0;
    subdwell_count = std::min<std::size_t>(32, subdwell_count);

    // Short saved fixtures and deliberately short TII dwells cannot satisfy
    // the configured independent-block requirement. Preserve the established
    // single-dwell decoder for those inputs; the normal 8 s monitor dwell is
    // split into four separately synchronized 2 s blocks.
    if (subdwell_count < settings.min_confirmed_subdwells) {
        auto result = decode_tii_single(samples, settings);
        result.subdwells_attempted = result.notifications ? 1 : 0;
        result.required_subdwells = result.subdwells_attempted;
        for (auto& candidate : result.candidates) {
            candidate.subdwell_observations = 1;
            candidate.subdwell_hits = candidate.confirmed ? 1 : 0;
            candidate.confidence = candidate.confirmed ? 1.0 : 0.0;
        }
        return result;
    }

    struct CandidateAggregate {
        std::size_t internal_hits = 0;
        std::size_t observations = 0;
        std::size_t confirmed_subdwells = 0;
        double level_sum = 0.0;
    };
    struct CollisionAggregate {
        std::size_t internal_hits = 0;
        std::size_t subdwells = 0;
        std::vector<std::uint8_t> possible_main_ids;
    };
    std::map<std::pair<std::uint8_t, std::uint8_t>, CandidateAggregate>
        candidates;
    using CollisionKey = std::tuple<std::uint8_t, std::uint8_t, std::uint8_t>;
    std::map<CollisionKey, CollisionAggregate> collisions;

    TiiInfo result;
    result.attempted = true;
    result.conservative = settings.conservative;
    result.dwell_ms = 1000.0 * samples.size() / sample_rate;
    result.subdwells_attempted = subdwell_count;
    result.required_subdwells = std::min(
        settings.min_confirmed_subdwells, subdwell_count);

    for (std::size_t part = 0; part < subdwell_count; ++part) {
        const auto begin = samples.size() * part / subdwell_count;
        const auto end = samples.size() * (part + 1) / subdwell_count;
        std::vector<std::complex<float>> block(
            samples.begin() + begin, samples.begin() + end);
        const auto decoded = decode_tii_single(block, settings);
        result.notifications += decoded.notifications;
        for (const auto& candidate : decoded.candidates) {
            auto& aggregate = candidates[
                {candidate.main_id, candidate.sub_id}];
            aggregate.internal_hits += candidate.hits;
            ++aggregate.observations;
            aggregate.confirmed_subdwells += candidate.confirmed ? 1 : 0;
            aggregate.level_sum += candidate.level;
        }
        for (const auto& collision : decoded.collisions) {
            const CollisionKey key{collision.sub_id, collision.active_mask,
                                   collision.active_blocks};
            auto& aggregate = collisions[key];
            aggregate.internal_hits += collision.hits;
            ++aggregate.subdwells;
            aggregate.possible_main_ids = collision.possible_main_ids;
        }
    }

    for (const auto& [code, aggregate] : candidates) {
        TiiCandidate candidate;
        candidate.main_id = code.first;
        candidate.sub_id = code.second;
        candidate.level = aggregate.observations
            ? aggregate.level_sum / aggregate.observations : 0.0;
        candidate.hits = aggregate.internal_hits;
        candidate.subdwell_hits = aggregate.confirmed_subdwells;
        candidate.subdwell_observations = aggregate.observations;
        candidate.confidence = subdwell_count
            ? double(aggregate.confirmed_subdwells) / subdwell_count : 0.0;
        candidate.confirmed = aggregate.confirmed_subdwells >=
            result.required_subdwells;
        result.candidates.push_back(candidate);
    }
    for (const auto& [key, aggregate] : collisions) {
        TiiCollision collision;
        collision.sub_id = std::get<0>(key);
        collision.active_mask = std::get<1>(key);
        collision.active_blocks = std::get<2>(key);
        collision.hits = aggregate.internal_hits;
        collision.confidence = subdwell_count
            ? double(aggregate.subdwells) / subdwell_count : 0.0;
        collision.possible_main_ids = aggregate.possible_main_ids;
        result.collisions.push_back(std::move(collision));
    }

    // A five-group collision can alternate between two four-group Main-ID
    // patterns as individual groups cross the occupancy threshold. This was
    // observed in the field as 3715 being confirmed in three subdwells while
    // 0715 appeared in the fourth: masks 8D and 2D have union AD, and AD was
    // independently measured as a persistent collision elsewhere in the
    // same dwell. In that situation the magnitude detector has no sound
    // basis for choosing either Main ID. Do not silently choose the more
    // persistent fit; retain it as an ambiguous diagnostic candidate.
    if (settings.conservative) {
        std::array<std::uint8_t, 70> patterns{};
        std::size_t pattern_count = 0;
        for (unsigned mask = 0; mask < 256; ++mask) {
            if (__builtin_popcount(mask) == 4)
                patterns[pattern_count++] = std::uint8_t(mask);
        }
        std::vector<bool> ambiguous(result.candidates.size(), false);
        for (std::size_t i = 0; i < result.candidates.size(); ++i) {
            if (!result.candidates[i].confirmed) continue;
            for (std::size_t j = 0; j < result.candidates.size(); ++j) {
                if (i == j ||
                    result.candidates[i].sub_id !=
                        result.candidates[j].sub_id ||
                    result.candidates[i].main_id ==
                        result.candidates[j].main_id)
                    continue;
                const auto combined_mask = std::uint8_t(
                    patterns[result.candidates[i].main_id] |
                    patterns[result.candidates[j].main_id]);
                if (__builtin_popcount(unsigned(combined_mask)) != 5)
                    continue;
                const bool measured_ambiguity = std::any_of(
                    result.collisions.begin(), result.collisions.end(),
                    [&](const auto& collision) {
                        if (collision.active_mask != combined_mask ||
                            collision.confidence <
                                settings.min_confirmed_confidence)
                            return false;
                        const auto contains = [&](std::uint8_t main_id) {
                            return std::find(
                                collision.possible_main_ids.begin(),
                                collision.possible_main_ids.end(), main_id) !=
                                collision.possible_main_ids.end();
                        };
                        return contains(result.candidates[i].main_id) &&
                            contains(result.candidates[j].main_id);
                    });
                if (measured_ambiguity) ambiguous[i] = true;
            }
        }
        for (std::size_t i = 0; i < result.candidates.size(); ++i) {
            if (!ambiguous[i]) continue;
            result.candidates[i].ambiguous = true;
            result.candidates[i].confirmed = false;
        }
    }
    std::sort(result.candidates.begin(), result.candidates.end(),
        [](const auto& lhs, const auto& rhs) {
            if (lhs.confirmed != rhs.confirmed)
                return lhs.confirmed > rhs.confirmed;
            if (lhs.subdwell_hits != rhs.subdwell_hits)
                return lhs.subdwell_hits > rhs.subdwell_hits;
            if (lhs.subdwell_observations != rhs.subdwell_observations)
                return lhs.subdwell_observations > rhs.subdwell_observations;
            return lhs.level > rhs.level;
        });
    std::sort(result.collisions.begin(), result.collisions.end(),
        [](const auto& lhs, const auto& rhs) {
            if (lhs.confidence != rhs.confidence)
                return lhs.confidence > rhs.confidence;
            if (lhs.sub_id != rhs.sub_id) return lhs.sub_id < rhs.sub_id;
            return lhs.active_mask < rhs.active_mask;
        });
    return result;
}

} // namespace dab::dx

// Streaming radio extension. Protocol geometry and bit ordering:
// ETSI EN 300 401 V2.1.1 (2017-01), 14.2, 14.4, 14.5 and 14.6.
// https://www.etsi.org/deliver/etsi_en/300400_300499/300401/02.01.01_60/en_300401v020101p.pdf
// Receiver choices (not ETSI requirements): 32-sample early FFT window,
// bounded reacquisition, fourth-power common-phase estimate, CRC-selected
// quadrant. These reuse our existing independent FIC receiver primitives.
namespace dab::dx
{
struct StreamDemodulator::Impl
{
    std::vector<std::complex<float>> buffer;
    IdentityAccumulator identity;
    bool locked = false, inverted = false, discontinuity = true, reconfiguring = false;
    int carrier = 0, quadrant = 0;
    double fine = 0;
    unsigned misses = 0;
    std::size_t anchor = 0;

    StreamFrame demodulate()
    {
        StreamFrame out;
        out.msc.resize(4 * 55296);
        std::array<float, 9216> fic{};
        // Estimate fractional CFO with cyclic prefixes; avoid symbol edges.
        std::complex<double> cp{};
        for (unsigned s = 0; s < 76; ++s)
        {
            auto start = anchor + null_size + s * symbol_size;
            for (unsigned i = 64; i < guard - 64; i += 4)
                cp += std::conj(std::complex<double>(buffer[start + i])) *
                      std::complex<double>(buffer[start + i + fft_size]);
        }
        if (std::abs(cp) > 1e-12)
            fine = std::arg(cp) * sample_rate / (2 * M_PI * fft_size);
        const auto prs = anchor + null_size + guard - 32;
        auto previous = symbol_fft(buffer, prs, fine, carrier, inverted);
        const auto &map = carrier_map();
        for (unsigned s = 0; s < 75; ++s)
        {
            auto current = symbol_fft(buffer, prs + (s + 1) * symbol_size, fine, carrier, inverted);
            std::array<std::complex<double>, 1536> d{};
            std::complex<double> fourth{};
            double power = 0;
            for (unsigned k = 0; k < 1536; ++k)
            {
                auto bin = (map[k] + 2048) % 2048;
                double rms = std::sqrt(std::norm(current[bin]) + std::norm(previous[bin]));
                d[k] = rms > 1e-12 ? current[bin] * std::conj(previous[bin]) / rms
                                   : std::complex<double>{};
                double a = std::abs(d[k]);
                if (a > 1e-12)
                {
                    auto u = d[k] / a;
                    fourth -= u * u * u * u * a;
                    power += a;
                }
            }
            auto rotation = std::polar(1., -0.25 * std::arg(fourth));
            float *target = s < 3 ? fic.data() + s * 3072 : out.msc.data() + (s - 3) * 3072;
            // Normalize once per symbol. Deep fades remain low-confidence;
            // clipping limits the influence of an impulsive carrier.
            double scale = power > 1e-12 ? 1536. / power : 0;
            for (unsigned k = 0; k < 1536; ++k)
            {
                auto v = d[k] * rotation * scale;
                target[k] = std::clamp(float(v.real()), -8.f, 8.f);
                target[1536 + k] = std::clamp(float(v.imag()), -8.f, 8.f);
            }
            previous = current;
        }
        auto rotate = [](float *p, unsigned count, int q)
        {
            for (unsigned s = 0; s < count; s += 3072)
                for (unsigned k = 0; k < 1536; ++k)
                {
                    float a = p[s + k], b = p[s + 1536 + k];
                    switch (q)
                    {
                    case 1:
                        p[s + k] = b;
                        p[s + 1536 + k] = -a;
                        break;
                    case 2:
                        p[s + k] = -a;
                        p[s + 1536 + k] = -b;
                        break;
                    case 3:
                        p[s + k] = -b;
                        p[s + 1536 + k] = a;
                        break;
                    }
                }
        };
        std::vector<std::array<std::uint8_t, 32>> best;
        // Prefer established orientation; only search others if FIC fails.
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            int q = (quadrant + attempt) % 4;
            auto trial = fic;
            rotate(trial.data(), 9216, q);
            std::vector<std::array<std::uint8_t, 32>> valid;
            for (unsigned w = 0; w < 4; ++w)
                for (auto &fib : decode_codeword(trial.data() + 2304 * w))
                    if (valid_fib_crc(fib))
                        valid.push_back(fib);
            if (valid.size() > best.size())
            {
                best = std::move(valid);
                quadrant = q;
                break;
            }
        }
        out.fic_valid = !best.empty();
        for (const auto &fib : best)
        {
            // Drop accumulated service maps when a different ensemble appears.
            for (unsigned pos = 0; pos < 30 && fib[pos] != 255;)
            {
                unsigned type = fib[pos] >> 5, len = fib[pos++] & 31;
                if (!len || pos + len > 30)
                    break;
                if (type == 0 && len >= 5 && (fib[pos] & 0xdf) == 0)
                {
                    unsigned eid = (fib[pos + 1] << 8) | fib[pos + 2];
                    if (identity.eid && *identity.eid != eid)
                    {
                        identity = {};
                        discontinuity = true;
                    }
                    // Reconfiguration is handled conservatively: stop using old
                    // routing until a current configuration is announced again.
                    const bool changing = (fib[pos + 3] & 0xc0) != 0;
                    if (changing != reconfiguring)
                    {
                        identity.subchannels.clear();
                        identity.services.clear();
                        discontinuity = true;
                    }
                    reconfiguring = changing;
                }
                pos += len;
            }
            ++identity.fic.valid_fibs;
            parse_fib(fib, identity);
        }
        DecodeResult decoded;
        update_decode_identity(decoded, identity);
        out.ensemble = std::move(decoded.ensemble);
        // EN 300 401 6.4: do not route an announced next configuration
        // onto current MSC bytes. Resume from freshly collected current FIGs.
        if (!reconfiguring)
            out.ensemble.metadata = mux_metadata(identity);
        rotate(out.msc.data(), out.msc.size(), quadrant);
        if (out.fic_valid)
            misses = 0;
        else
            ++misses;
        out.discontinuity = discontinuity;
        discontinuity = false;
        return out;
    }
};
StreamDemodulator::StreamDemodulator() : impl_(std::make_unique<Impl>()) {}
StreamDemodulator::~StreamDemodulator() = default;
void StreamDemodulator::reset() { impl_ = std::make_unique<Impl>(); }
void StreamDemodulator::feed(const std::complex<float> *iq, std::size_t count,
                             const std::function<void(StreamFrame &&)> &output)
{
    if (!iq || !count)
        return;
    auto &p = *impl_;
    // Accept arbitrary caller chunks without unbounded accumulation.
    while (count)
    {
        auto n = std::min(count, std::size_t(32768));
        p.buffer.insert(p.buffer.end(), iq, iq + n);
        iq += n;
        count -= n;
        for (;;)
        {
            if (!p.locked)
            {
                // Two frames bound the wait for a complete frame at any phase.
                // This is an acquisition policy, not an ETSI requirement;
                // validate timing separately from CRC-valid FIC and identity.
                if (p.buffer.size() < 2 * frame_size)
                    break;
                auto detection = detect_mode_i(p.buffer, {});
                FicDecodeSettings settings(false);
                settings.two_dimensional_fallback = false;
                auto candidate = strongest_phase_candidate(p.buffer, detection, settings);
                if (!candidate)
                {
                    p.buffer.erase(p.buffer.begin(), p.buffer.begin() + frame_size);
                    continue;
                }
                p.anchor = candidate->phase;
                p.carrier = candidate->carrier_offset;
                p.inverted = candidate->inverted;
                p.fine = detection.fine_offset_hz;
                p.locked = true;
                p.misses = 0;
                p.discontinuity = true;
            }
            if (p.anchor + frame_size + 64 > p.buffer.size())
                break;
            // Slowly track sample-clock drift with CP correlation. This is a
            // local receiver heuristic; it does not alter the transmitted bits.
            int shift = 0;
            double best = -1;
            for (int delta = -8; delta <= 8; ++delta)
            {
                if (std::int64_t(p.anchor) + delta < 0)
                    continue;
                std::complex<double> sum{};
                double energy = 0;
                for (unsigned s = 0; s < 76; s += 4)
                    for (unsigned i = 16; i < guard - 16; i += 8)
                    {
                        auto at = std::size_t(std::int64_t(p.anchor) + delta) + null_size +
                                  s * symbol_size + i;
                        auto a = p.buffer[at], b = p.buffer[at + fft_size];
                        sum += std::conj(std::complex<double>(a)) * std::complex<double>(b);
                        energy += std::norm(a) + std::norm(b);
                    }
                double score = energy > 0 ? std::abs(sum) / energy : 0;
                if (score > best)
                {
                    best = score;
                    shift = delta;
                }
            }
            p.anchor = std::size_t(std::int64_t(p.anchor) + shift);
            auto frame = p.demodulate();
            output(std::move(frame));
            const auto consumed = p.anchor + frame_size;
            // Keep a small margin so the next timing estimate can move earlier.
            const auto discard = consumed - 64;
            p.buffer.erase(p.buffer.begin(), p.buffer.begin() + discard);
            p.anchor = 64;
            if (p.misses >= 8)
            {
                p.locked = false;
                p.discontinuity = true;
                p.identity = {};
                p.reconfiguring = false;
            }
        }
    }
}
} // namespace dab::dx
