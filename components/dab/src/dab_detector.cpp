#include "dab_detector.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace dab {
namespace {

constexpr std::size_t useful = 2048;
constexpr std::size_t guard = 504;
constexpr std::size_t symbol = useful + guard;
constexpr std::size_t scan_step = 8;
constexpr std::size_t null_length = 2656;

double median(std::vector<double> values)
{
    if (values.empty()) return 0.0;
    auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    double result = *middle;
    if (values.size() % 2 == 0) {
        auto lower = std::max_element(values.begin(), middle);
        result = (*lower + result) / 2.0;
    }
    return result;
}

std::int64_t positive_mod(std::int64_t value, std::int64_t modulus)
{
    value %= modulus;
    return value < 0 ? value + modulus : value;
}

std::int64_t wrapped_delta(std::int64_t value, std::int64_t reference,
                           std::int64_t modulus)
{
    auto delta = positive_mod(value - reference, modulus);
    if (delta > modulus / 2) delta -= modulus;
    return delta;
}

struct Peak {
    std::size_t sample = 0;
    double metric = 0.0;
    double phase = 0.0;
};

void fft(std::vector<std::complex<double>>& values, bool inverse)
{
    const std::size_t count = values.size();
    for (std::size_t i = 1, j = 0; i < count; ++i) {
        std::size_t bit = count >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }
    for (std::size_t length = 2; length <= count; length <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * M_PI / length;
        const std::complex<double> root(std::cos(angle), std::sin(angle));
        for (std::size_t begin = 0; begin < count; begin += length) {
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
    if (inverse) {
        for (auto& value : values) value /= double(count);
    }
}

std::vector<std::complex<float>> make_mode_i_prs_reference()
{
    // ETSI EN 300 401 V2.1.1 clause 14.3.2, tables 23 and 24.
    constexpr int h[4][16] = {
        {0,2,0,0,0,0,1,1,2,0,0,0,2,2,1,1},
        {0,3,2,3,0,1,3,0,2,1,2,3,2,3,3,0},
        {0,0,0,2,0,2,1,3,2,2,0,2,2,0,1,3},
        {0,1,2,1,0,3,3,2,2,3,2,1,2,1,3,2},
    };
    constexpr int negative_n[24] = {
        1,2,0,1,3,2,2,3,2,1,2,3,1,2,3,3,2,2,2,1,1,3,1,2};
    constexpr int positive_n[24] = {
        3,1,1,1,2,2,1,0,2,2,3,3,0,2,1,3,3,3,3,0,3,0,1,1};
    const std::array<std::complex<double>, 4> quadrant = {
        std::complex<double>(1.0, 0.0), {0.0, 1.0},
        {-1.0, 0.0}, {0.0, -1.0}};
    std::vector<std::complex<double>> spectrum(useful);
    for (int carrier = -768; carrier <= 768; ++carrier) {
        if (carrier == 0) continue;
        int block = 0, kprime = 0, row = 0, n = 0;
        if (carrier < 0) {
            block = (carrier + 768) / 32;
            kprime = -768 + 32 * block;
            row = block % 4;
            n = negative_n[block];
        } else {
            block = (carrier - 1) / 32;
            kprime = 1 + 32 * block;
            row = (4 - block % 4) % 4;
            n = positive_n[block];
        }
        const int phase = (h[row][(carrier - kprime) % 16] + n) % 4;
        const std::size_t bin = carrier < 0 ? useful + carrier : carrier;
        spectrum[bin] = quadrant[phase];
    }
    fft(spectrum, true);
    std::vector<std::complex<float>> result(useful);
    for (std::size_t index = 0; index < useful; ++index)
        result[index] = {float(spectrum[index].real()), float(spectrum[index].imag())};
    return result;
}

std::vector<std::complex<float>> prs_bandlimited(
    const std::vector<std::complex<float>>& samples, std::size_t count)
{
    // The Mode-I carriers end at +/-768 kHz.  A short symmetric low-pass
    // removes energy near the 1.024 MHz Nyquist edge before normalized PRS
    // correlation.  Its centred, non-causal application preserves sample
    // indices, which is essential for timing-anchor reporting.
    constexpr int half = 16;
    constexpr double cutoff = 0.40; // cycles/sample: 819.2 kHz at 2.048 MS/s
    std::array<double, 2 * half + 1> taps{};
    double tap_sum = 0.0;
    for (int index = -half; index <= half; ++index) {
        const double ideal = index == 0 ? 2.0 * cutoff
            : std::sin(2.0 * M_PI * cutoff * index) / (M_PI * index);
        const double window = 0.54 + 0.46 *
            std::cos(M_PI * index / double(half));
        taps[std::size_t(index + half)] = ideal * window;
        tap_sum += ideal * window;
    }
    for (auto& tap : taps) tap /= tap_sum;

    count = std::min(count, samples.size());
    std::vector<std::complex<float>> filtered(count);
    for (std::size_t n = 0; n < count; ++n) {
        std::complex<double> sum;
        const int begin = std::max(-half, -int(n));
        const int end = std::min(half, int(samples.size() - 1 - n));
        for (int offset = begin; offset <= end; ++offset)
            sum += double(taps[std::size_t(offset + half)]) *
                std::complex<double>(samples[std::size_t(std::int64_t(n) + offset)]);
        filtered[n] = {float(sum.real()), float(sum.imag())};
    }
    return filtered;
}

void refine_prs(const std::vector<std::complex<float>>& samples,
                const TimingSearchConfig& config,
                std::vector<TimingCandidate>& candidates)
{
    const auto cancelled = [&] {
        return config.cancel_requested && config.cancel_requested();
    };
    if (cancelled()) return;
    if (config.prs_fine_step_samples == 0) return;
    const auto& reference = mode_i_prs_reference();
    std::vector<std::complex<double>> reference_spectrum(useful);
    for (std::size_t index = 0; index < useful; ++index)
        reference_spectrum[index] = reference[index];
    fft(reference_spectrum, false);

    constexpr std::int64_t frame = 196608;
    for (auto& candidate : candidates) {
        if (cancelled()) return;
        const std::int64_t candidate_phase =
            std::int64_t(candidate.frame_phase_sample);
        std::vector<std::vector<std::complex<double>>> received_spectra;
        // A long DX capture may contain only a short aircraft/tropo peak near
        // its end. Rank the complete frame raster cheaply first and spend PRS
        // FFT work only on its strongest passages instead of the first N.
        std::vector<std::size_t> selected_frames;
        if (config.prs_temporal_rank) {
            const auto ranked_frames = rank_mode_i_frames(
                samples, std::size_t(positive_mod(candidate_phase, frame)),
                config.prs_max_frames, config.cancel_requested);
            for (const auto& ranked : ranked_frames)
                selected_frames.push_back(ranked.frame_start_sample);
        } else {
            for (std::int64_t null_start = candidate_phase;
                 null_start < std::int64_t(samples.size()) &&
                     selected_frames.size() < config.prs_max_frames;
                 null_start += frame) {
                if (null_start >= 0 &&
                    null_start + frame <= std::int64_t(samples.size()))
                    selected_frames.push_back(std::size_t(null_start));
            }
        }
        for (const auto selected_frame : selected_frames) {
            if (cancelled()) return;
            const auto null_start = std::int64_t(selected_frame);
            const std::int64_t prs_start =
                null_start + std::int64_t(null_length + guard);
            if (prs_start < 0 || prs_start + std::int64_t(useful) >
                std::int64_t(samples.size()))
                continue;
            std::vector<std::complex<double>> spectrum(useful);
            for (std::size_t index = 0; index < useful; ++index)
                spectrum[index] = samples[std::size_t(prs_start) + index];
            fft(spectrum, false);
            received_spectra.push_back(std::move(spectrum));
        }
        double best_metric = -1.0;
        std::int64_t best_delay = 0;
        int best_carrier_offset = 0;
        bool best_inverted = false;
        const auto radius = std::int64_t(config.prs_refine_radius_samples);
        const auto step = std::int64_t(config.prs_fine_step_samples);
        // A correctly referenced B210 normally selects bin zero and the normal
        // spectrum. The bounded alternatives make coarse carrier slips and IQ
        // inversion explicit instead of silently rejecting a valid PRS.
        for (bool inverted : {false, true}) {
            for (int carrier_offset = -8; carrier_offset <= 8; ++carrier_offset) {
                if (cancelled()) return;
                std::vector<double> impulse_power(useful, 0.0);
                for (const auto& received : received_spectra) {
                    std::vector<std::complex<double>> response(useful);
                    for (int carrier = -768; carrier <= 768; ++carrier) {
                        if (carrier == 0) continue;
                        const int observed_carrier = inverted
                            ? -carrier + carrier_offset
                            : carrier + carrier_offset;
                        const auto observed_bin = std::size_t(
                            (observed_carrier % int(useful) + int(useful)) % int(useful));
                        const auto reference_bin = std::size_t(
                            (carrier % int(useful) + int(useful)) % int(useful));
                        const auto observed = inverted
                            ? std::conj(received[observed_bin])
                            : received[observed_bin];
                        response[reference_bin] = observed *
                            std::conj(reference_spectrum[reference_bin]);
                    }
                    fft(response, true);
                    for (std::size_t index = 0; index < useful; ++index)
                        impulse_power[index] += std::norm(response[index]);
                }
                const double total_power = std::accumulate(
                    impulse_power.begin(), impulse_power.end(), 0.0);
                if (total_power <= 0.0) continue;
                for (std::int64_t delay = -radius; delay <= radius; delay += step) {
                    const auto index = std::size_t(
                        (delay % std::int64_t(useful) + std::int64_t(useful)) %
                        std::int64_t(useful));
                    const double metric = std::sqrt(impulse_power[index] / total_power);
                    if (metric > best_metric) {
                        best_metric = metric;
                        best_delay = delay;
                        best_carrier_offset = carrier_offset;
                        best_inverted = inverted;
                    }
                }
            }
        }
        candidate.prs_refined_offset_samples =
            candidate.offset_samples + best_delay;
        candidate.prs_refined_offset_us = 1.0e6 *
            candidate.prs_refined_offset_samples / config.sample_rate;
        candidate.prs_metric = std::max(0.0, best_metric);
        candidate.prs_frames = received_spectra.size();
        candidate.prs_confirmed = !received_spectra.empty() &&
            candidate.prs_metric >= config.prs_min_metric;
        candidate.prs_integer_carrier_offset = best_carrier_offset;
        candidate.prs_spectrum_inverted = best_inverted;

        if (!candidate.prs_confirmed) continue;

        // Re-evaluate the selected carrier/orientation hypothesis and retain
        // the non-coherently averaged PRS channel impulse response. The older
        // implementation used this response only to select best_delay and
        // discarded the remaining paths.
        std::vector<double> impulse_power(useful, 0.0);
        for (const auto& received : received_spectra) {
            if (cancelled()) return;
            std::vector<std::complex<double>> response(useful);
            for (int carrier = -768; carrier <= 768; ++carrier) {
                if (carrier == 0) continue;
                const int observed_carrier = best_inverted
                    ? -carrier + best_carrier_offset
                    : carrier + best_carrier_offset;
                const auto observed_bin = std::size_t(
                    (observed_carrier % int(useful) + int(useful)) % int(useful));
                const auto reference_bin = std::size_t(
                    (carrier % int(useful) + int(useful)) % int(useful));
                const auto observed = best_inverted
                    ? std::conj(received[observed_bin])
                    : received[observed_bin];
                response[reference_bin] = observed *
                    std::conj(reference_spectrum[reference_bin]);
            }
            fft(response, true);
            for (std::size_t index = 0; index < useful; ++index)
                impulse_power[index] += std::norm(response[index]);
        }
        if (received_spectra.empty()) continue;
        for (auto& power : impulse_power) power /= received_spectra.size();
        const double maximum_power = *std::max_element(
            impulse_power.begin(), impulse_power.end());
        if (maximum_power <= 0.0) continue;

        std::vector<double> levels(useful);
        std::transform(impulse_power.begin(), impulse_power.end(), levels.begin(),
            [&](double power) { return std::sqrt(power / maximum_power); });
        const double noise_level = median(levels);
        const double peak_threshold = std::max(
            config.cir_peak_relative_threshold,
            config.cir_peak_noise_ratio * noise_level);

        struct RankedPeak {
            std::int64_t delay = 0;
            double level = 0.0;
        };
        std::vector<RankedPeak> ranked;
        const auto best_index = std::size_t(positive_mod(best_delay, useful));
        for (std::size_t index = 0; index < useful; ++index) {
            const auto left = (index + useful - 1) % useful;
            const auto right = (index + 1) % useful;
            if (levels[index] < peak_threshold ||
                levels[index] < levels[left] || levels[index] < levels[right])
                continue;
            const auto relative = wrapped_delta(
                std::int64_t(index), std::int64_t(best_index), useful);
            ranked.push_back({best_delay + relative, levels[index]});
        }
        std::sort(ranked.begin(), ranked.end(), [](const auto& lhs,
                                                   const auto& rhs) {
            return lhs.level > rhs.level;
        });
        std::vector<RankedPeak> separated;
        for (const auto& peak : ranked) {
            const bool too_close = std::any_of(
                separated.begin(), separated.end(), [&](const auto& prior) {
                    return std::llabs(peak.delay - prior.delay) <
                        std::int64_t(config.cir_peak_min_separation_samples);
                });
            if (!too_close) separated.push_back(peak);
            if (separated.size() == config.cir_max_peaks) break;
        }
        if (separated.empty()) separated.push_back({best_delay, 1.0});

        candidate.cir_available = true;
        candidate.cir_frames = received_spectra.size();
        candidate.cir_noise_relative_level = noise_level;
        candidate.cir_strongest_path_offset_samples = separated.front().delay;
        std::sort(separated.begin(), separated.end(), [](const auto& lhs,
                                                         const auto& rhs) {
            return lhs.delay < rhs.delay;
        });
        for (const auto& peak : separated)
            candidate.cir_peaks.push_back({
                peak.delay, 1.0e6 * peak.delay / config.sample_rate,
                peak.level});

        // Find the guard-sized interval containing the greatest summed path
        // power. It is the discrete counterpart of QIRX's common guard
        // overlap. Place the FFT start halfway through the remaining valid
        // interval: [latest path - guard, earliest path]. A path delayed by d
        // has a cyclic prefix from d-guard through d, so an FFT start inside
        // that intersection remains free of previous/next-symbol energy.
        double best_cluster_power = -1.0;
        std::int64_t cluster_min = separated.front().delay;
        std::int64_t cluster_max = separated.front().delay;
        for (std::size_t begin = 0; begin < separated.size(); ++begin) {
            double power = 0.0;
            for (std::size_t end = begin; end < separated.size(); ++end) {
                if (separated[end].delay - separated[begin].delay >
                    std::int64_t(guard))
                    break;
                power += separated[end].level * separated[end].level;
                if (power > best_cluster_power) {
                    best_cluster_power = power;
                    cluster_min = separated[begin].delay;
                    cluster_max = separated[end].delay;
                }
            }
        }
        candidate.cir_cluster_min_delay_samples = cluster_min;
        candidate.cir_cluster_max_delay_samples = cluster_max;
        candidate.cir_guard_overlap_samples =
            std::int64_t(guard) - (cluster_max - cluster_min);
        candidate.cir_isi_free = candidate.cir_guard_overlap_samples >= 0;
        candidate.cir_quasi_optimal_offset_samples =
            (cluster_max + cluster_min - std::int64_t(guard)) / 2;
    }
}

std::vector<TimingCandidate> find_full_frame_prs(
    const std::vector<std::complex<float>>& samples,
    std::size_t base_null_start_sample, const TimingSearchConfig& config)
{
    const auto cancelled = [&] {
        return config.cancel_requested && config.cancel_requested();
    };
    if (cancelled()) return {};
    constexpr std::int64_t frame = 196608;
    constexpr std::size_t fft_size = 262144;
    if (samples.size() < std::size_t(frame + useful)) return {};
    const std::size_t segment_size = std::size_t(frame) + useful - 1;
    const auto required_samples = std::min(
        samples.size(), config.prs_max_frames * std::size_t(frame) + useful - 1);
    const auto filtered = prs_bandlimited(samples, required_samples);
    if (cancelled()) return {};
    const auto& reference = mode_i_prs_reference();
    double reference_energy = 0.0;
    std::vector<std::complex<double>> kernel(fft_size);
    for (std::size_t n = 0; n < useful; ++n) {
        reference_energy += std::norm(reference[n]);
        kernel[useful - 1 - n] =
            std::conj(std::complex<double>(reference[n]));
    }
    fft(kernel, false);

    struct PhasePower {
        double sum = 0.0;
        std::size_t count = 0;
        bool inverted = false;
    };
    std::array<std::vector<PhasePower>, 2> phases = {
        std::vector<PhasePower>(frame), std::vector<PhasePower>(frame)};
    for (std::size_t block = 0; block < config.prs_max_frames; ++block) {
        if (cancelled()) return {};
        const std::size_t block_start = block * std::size_t(frame);
        if (block_start + segment_size > samples.size()) break;
        for (int orientation = 0; orientation < 2; ++orientation) {
            if (cancelled()) return {};
            std::vector<std::complex<double>> transformed(fft_size);
            std::vector<double> energy_prefix(segment_size + 1);
            for (std::size_t n = 0; n < segment_size; ++n) {
                auto value = std::complex<double>(filtered[block_start + n]);
                if (orientation) value = std::conj(value);
                transformed[n] = value;
                energy_prefix[n + 1] = energy_prefix[n] + std::norm(value);
            }
            fft(transformed, false);
            if (cancelled()) return {};
            for (std::size_t n = 0; n < fft_size; ++n)
                transformed[n] *= kernel[n];
            fft(transformed, true);
            if (cancelled()) return {};
            for (std::size_t phase = 0; phase < std::size_t(frame); ++phase) {
                const double sample_energy =
                    energy_prefix[phase + useful] - energy_prefix[phase];
                if (sample_energy <= 0.0) continue;
                const double metric = std::abs(transformed[phase + useful - 1]) /
                    std::sqrt(reference_energy * sample_energy);
                phases[orientation][phase].sum += metric * metric;
                ++phases[orientation][phase].count;
                phases[orientation][phase].inverted = orientation != 0;
            }
        }
    }

    struct PeakScore {
        std::size_t prs_phase = 0;
        double metric = 0.0;
        std::size_t frames = 0;
        bool inverted = false;
    };
    std::vector<PeakScore> peaks;
    for (int orientation = 0; orientation < 2; ++orientation) {
        if (cancelled()) return {};
        const auto& values = phases[orientation];
        for (std::size_t phase = 0; phase < std::size_t(frame); ++phase) {
            if (values[phase].count == 0) continue;
            const double metric = std::sqrt(
                values[phase].sum / values[phase].count);
            const auto left = phase == 0 ? std::size_t(frame - 1) : phase - 1;
            const auto right = phase + 1 == std::size_t(frame) ? 0 : phase + 1;
            const double left_metric = values[left].count ?
                std::sqrt(values[left].sum / values[left].count) : 0.0;
            const double right_metric = values[right].count ?
                std::sqrt(values[right].sum / values[right].count) : 0.0;
            if (metric >= config.prs_full_frame_min_metric &&
                metric >= left_metric &&
                metric >= right_metric)
                peaks.push_back({phase, metric, values[phase].count,
                                 orientation != 0});
        }
    }
    std::sort(peaks.begin(), peaks.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.metric > rhs.metric;
    });
    std::vector<PeakScore> separated;
    for (const auto& peak : peaks) {
        bool keep = true;
        for (const auto& prior : separated) {
            auto distance = std::llabs(std::int64_t(peak.prs_phase) -
                                       std::int64_t(prior.prs_phase));
            distance = std::min(distance, frame - distance);
            // A truncated window overlapping the real PRS creates deterministic
            // partial-correlation lobes for roughly one complete OFDM symbol.
            // Nearby hypotheses are already handled by the local CP/PRS path;
            // the full-frame fallback groups this entire ambiguity region.
            if (distance <= std::int64_t(symbol)) {
                keep = false;
                break;
            }
        }
        if (keep) separated.push_back(peak);
        if (separated.size() == config.max_candidates) break;
    }

    const auto base_prs_phase = positive_mod(
        std::int64_t(base_null_start_sample) +
            std::int64_t(null_length + guard), frame);
    std::vector<TimingCandidate> result;
    for (const auto& peak : separated) {
        auto offset = wrapped_delta(std::int64_t(peak.prs_phase),
                                    base_prs_phase, frame);
        const auto null_phase = positive_mod(
            std::int64_t(peak.prs_phase) -
                std::int64_t(null_length + guard), frame);
        TimingCandidate candidate;
        candidate.offset_samples = offset;
        candidate.offset_us = 1.0e6 * offset / config.sample_rate;
        candidate.frame_phase_sample = std::size_t(null_phase);
        candidate.dominant_cluster =
            std::llabs(offset) <= std::int64_t(guard);
        candidate.prs_refined_offset_samples = offset;
        candidate.prs_refined_offset_us = candidate.offset_us;
        candidate.prs_metric = peak.metric;
        candidate.prs_frames = peak.frames;
        candidate.prs_confirmed = true;
        candidate.prs_spectrum_inverted = peak.inverted;
        result.push_back(std::move(candidate));
    }
    return result;
}

} // namespace

const std::vector<std::complex<float>>& mode_i_prs_reference()
{
    static const auto reference = make_mode_i_prs_reference();
    return reference;
}

Detection detect_mode_i(const std::vector<std::complex<float>>& samples,
                        const DetectorConfig& config)
{
    if (std::abs(config.sample_rate - 2048000.0) > 2.0)
        throw std::runtime_error("Mode-I detector requires 2.048 MS/s");
    if (config.cp_threshold <= 0.0 || config.cp_threshold >= 1.0)
        throw std::runtime_error("cp_threshold must be between 0 and 1");

    Detection result;
    if (samples.empty()) return result;

    long double total_power = 0.0;
    for (const auto& value : samples) total_power += std::norm(value);
    double mean_power = double(total_power / samples.size());
    result.rms_dbfs = 10.0 * std::log10(std::max(mean_power, 1e-20));
    result.energy_present = result.rms_dbfs > -100.0;
    if (samples.size() < symbol + guard) return result;

    if (samples.size() >= null_length) {
        std::vector<double> power_sum(samples.size() + 1);
        for (std::size_t i = 0; i < samples.size(); ++i)
            power_sum[i + 1] = power_sum[i] + std::norm(samples[i]);
        std::vector<double> windows;
        double minimum = std::numeric_limits<double>::infinity();
        std::size_t minimum_start = 0;
        for (std::size_t start = 0; start + null_length <= samples.size(); start += 128) {
            double power = (power_sum[start + null_length] - power_sum[start]) / null_length;
            windows.push_back(power);
            if (power < minimum) {
                minimum = power;
                minimum_start = start;
            }
        }
        const std::size_t refine_begin = minimum_start > 127 ? minimum_start - 127 : 0;
        const std::size_t refine_end = std::min(
            samples.size() - null_length, minimum_start + 127);
        for (std::size_t start = refine_begin; start <= refine_end; ++start) {
            const double power =
                (power_sum[start + null_length] - power_sum[start]) / null_length;
            if (power < minimum) {
                minimum = power;
                minimum_start = start;
            }
        }
        double typical = median(windows);
        result.null_depth_db = 10.0 * std::log10(std::max(typical, 1e-20) /
                                                 std::max(minimum, 1e-20));
        result.null_sync = result.null_depth_db >= config.min_null_depth_db;
        result.null_start_sample = minimum_start;
    }

    const std::size_t products = samples.size() - useful;
    std::vector<std::complex<double>> corr_sum(products + 1);
    std::vector<double> energy_sum(products + 1);
    for (std::size_t i = 0; i < products; ++i) {
        corr_sum[i + 1] = corr_sum[i] +
            std::conj(std::complex<double>(samples[i])) * std::complex<double>(samples[i + useful]);
        energy_sum[i + 1] = energy_sum[i] + std::norm(samples[i]) + std::norm(samples[i + useful]);
    }

    std::vector<Peak> peaks;
    bool in_region = false;
    Peak best;
    auto finish_region = [&]() {
        if (!in_region) return;
        if (peaks.empty() || best.sample - peaks.back().sample >= 1800)
            peaks.push_back(best);
        else if (best.metric > peaks.back().metric)
            peaks.back() = best;
        in_region = false;
    };

    for (std::size_t start = 0; start + guard <= products; start += scan_step) {
        auto corr = corr_sum[start + guard] - corr_sum[start];
        double energy = energy_sum[start + guard] - energy_sum[start];
        double metric = energy > 0.0 ? 2.0 * std::abs(corr) / energy : 0.0;
        if (metric >= config.cp_threshold) {
            if (!in_region || metric > best.metric)
                best = {start, metric, std::arg(corr)};
            in_region = true;
        } else {
            finish_region();
        }
    }
    finish_region();

    // Strong real-world DAB captures can remain highly correlated at the
    // 2048-sample lag between the narrow null regions. In that case the whole
    // scan is one region; take independent observations and require the null
    // depth below to reject a tone or other stationary periodic interferer.
    if (peaks.size() < config.min_cp_peaks) {
        peaks.clear();
        for (std::size_t start = 0; start + guard <= products; start += 2048) {
            auto corr = corr_sum[start + guard] - corr_sum[start];
            double energy = energy_sum[start + guard] - energy_sum[start];
            double metric = energy > 0.0 ? 2.0 * std::abs(corr) / energy : 0.0;
            if (metric >= config.cp_threshold)
                peaks.push_back({start, metric, std::arg(corr)});
        }
    }

    result.cp_peaks = peaks.size();
    result.phase_sync = peaks.size() >= config.min_cp_peaks && result.null_sync;
    if (peaks.empty()) return result;

    std::vector<double> metrics, offsets;
    metrics.reserve(peaks.size());
    offsets.reserve(peaks.size());
    for (const auto& peak : peaks) {
        metrics.push_back(peak.metric);
        offsets.push_back(peak.phase * config.sample_rate / (2.0 * M_PI * useful));
        result.cp_metric_max = std::max(result.cp_metric_max, peak.metric);
    }
    result.cp_metric_median = median(metrics);
    result.fine_offset_hz = median(offsets);
    result.lock_time_ms = 1000.0 * peaks.front().sample / config.sample_rate;
    double rho = std::clamp(result.cp_metric_median, 1e-6, 0.999999);
    result.estimated_snr_db = 10.0 * std::log10(rho / (1.0 - rho));
    return result;
}

std::vector<TimingCandidate> find_mode_i_timing_candidates(
    const std::vector<std::complex<float>>& samples,
    std::size_t base_null_start_sample, const TimingSearchConfig& config)
{
    const auto cancelled = [&] {
        return config.cancel_requested && config.cancel_requested();
    };
    if (cancelled()) return {};
    constexpr std::int64_t frame = 196608;
    if (std::abs(config.sample_rate - 2048000.0) > 2.0)
        throw std::runtime_error("Mode-I timing search requires 2.048 MS/s");
    if (config.coarse_step_samples == 0 || config.fine_step_samples == 0 ||
        config.max_candidates == 0 || config.min_separation_samples == 0)
        return {};
    if (samples.size() < std::size_t(frame)) return {};

    const std::size_t products = samples.size() - useful;
    std::vector<std::complex<double>> corr_sum(products + 1);
    std::vector<double> energy_sum(products + 1);
    std::vector<double> power_sum(samples.size() + 1);
    for (std::size_t i = 0; i < products; ++i) {
        if ((i & 0x3ffffU) == 0 && cancelled()) return {};
        corr_sum[i + 1] = corr_sum[i] +
            std::conj(std::complex<double>(samples[i])) *
            std::complex<double>(samples[i + useful]);
        energy_sum[i + 1] = energy_sum[i] +
            std::norm(samples[i]) + std::norm(samples[i + useful]);
    }
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if ((i & 0x3ffffU) == 0 && cancelled()) return {};
        power_sum[i + 1] = power_sum[i] + std::norm(samples[i]);
    }

    auto normalized_phase = [&](std::int64_t value) {
        value %= frame;
        if (value < 0) value += frame;
        return value;
    };
    const std::int64_t base_phase = normalized_phase(
        std::int64_t(base_null_start_sample));
    struct AnchorScore {
        double cp_metric = 0.0;
        double null_depth_db = 0.0;
        double rank = 0.0;
    };
    auto score = [&](std::int64_t offset) {
        const std::int64_t phase = normalized_phase(base_phase + offset);
        std::vector<double> metrics;
        std::vector<double> null_depths;
        for (std::int64_t frame_start = phase;
             frame_start < std::int64_t(samples.size()); frame_start += frame) {
            if (frame_start + std::int64_t(null_length + symbol) <=
                std::int64_t(samples.size())) {
                const auto start = std::size_t(frame_start);
                const double null_power =
                    (power_sum[start + null_length] - power_sum[start]) / null_length;
                const double data_power =
                    (power_sum[start + null_length + symbol] -
                     power_sum[start + null_length]) / symbol;
                null_depths.push_back(10.0 * std::log10(
                    std::max(data_power, 1e-20) / std::max(null_power, 1e-20)));
            }
            for (std::size_t index = 0; index < 76; ++index) {
                const std::int64_t start = frame_start + std::int64_t(null_length) +
                    std::int64_t(index * symbol);
                if (start < 0 || start + std::int64_t(guard) >
                    std::int64_t(products))
                    continue;
                const auto begin = std::size_t(start);
                const auto corr = corr_sum[begin + guard] - corr_sum[begin];
                const double energy = energy_sum[begin + guard] - energy_sum[begin];
                metrics.push_back(energy > 0.0 ?
                    2.0 * std::abs(corr) / energy : 0.0);
            }
        }
        AnchorScore result;
        result.cp_metric = median(std::move(metrics));
        result.null_depth_db = median(std::move(null_depths));
        result.rank = result.cp_metric * std::max(0.05, result.null_depth_db);
        return result;
    };

    struct ScoredOffset {
        std::int64_t offset;
        AnchorScore score;
    };
    std::vector<ScoredOffset> coarse;
    const auto maximum = std::int64_t(std::llround(
        config.max_delay_us * config.sample_rate / 1.0e6));
    const auto coarse_step = std::int64_t(config.coarse_step_samples);
    for (std::int64_t offset = -maximum; offset <= maximum; offset += coarse_step)
    {
        if (cancelled()) return {};
        coarse.push_back({offset, score(offset)});
    }
    if (coarse.empty() || coarse.back().offset != maximum)
        coarse.push_back({maximum, score(maximum)});
    std::sort(coarse.begin(), coarse.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.score.rank > rhs.score.rank;
    });

    std::vector<ScoredOffset> selected;
    for (const auto& candidate : coarse) {
        bool separate = true;
        for (const auto& prior : selected) {
            if (std::llabs(candidate.offset - prior.offset) <=
                std::int64_t(config.min_separation_samples)) {
                separate = false;
                break;
            }
        }
        if (!separate) continue;
        selected.push_back(candidate);
        if (selected.size() == config.max_candidates) break;
    }

    std::vector<TimingCandidate> result;
    const auto fine_step = std::int64_t(config.fine_step_samples);
    for (auto candidate : selected) {
        if (cancelled()) return {};
        const auto begin = std::max(-maximum, candidate.offset - coarse_step);
        const auto end = std::min(maximum, candidate.offset + coarse_step);
        for (std::int64_t offset = begin; offset <= end; offset += fine_step) {
            const auto evaluated = score(offset);
            if (evaluated.rank > candidate.score.rank)
                candidate = {offset, evaluated};
        }
        const bool dominant_cluster =
            std::llabs(candidate.offset) <= std::int64_t(guard);
        if (!dominant_cluster &&
            (candidate.score.cp_metric < 0.15 ||
             candidate.score.null_depth_db < 1.5))
            continue;
        TimingCandidate timing;
        timing.offset_samples = candidate.offset;
        timing.offset_us = 1.0e6 * candidate.offset / config.sample_rate;
        timing.frame_phase_sample = std::size_t(
            normalized_phase(base_phase + candidate.offset));
        timing.cp_metric = candidate.score.cp_metric;
        timing.null_depth_db = candidate.score.null_depth_db;
        timing.dominant_cluster = dominant_cluster;
        result.push_back(std::move(timing));
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        const double lhs_rank = lhs.cp_metric * std::max(0.05, lhs.null_depth_db);
        const double rhs_rank = rhs.cp_metric * std::max(0.05, rhs.null_depth_db);
        return lhs_rank > rhs_rank;
    });
    std::vector<TimingCandidate> separated;
    for (const auto& candidate : result) {
        bool keep = true;
        for (const auto& prior : separated) {
            if (std::llabs(candidate.offset_samples - prior.offset_samples) <=
                std::int64_t(config.min_separation_samples)) {
                keep = false;
                break;
            }
        }
        if (keep) separated.push_back(candidate);
    }
    refine_prs(samples, config, separated);
    if (cancelled()) return {};
    // Full-frame correlation can either be a bounded fallback or run beside a
    // local confirmation to expose a second, differently timed PRS.
    const bool local_prs_confirmed = std::any_of(
        separated.begin(), separated.end(),
        [](const auto& candidate) { return candidate.prs_confirmed; });
    auto full_frame = local_prs_confirmed && !config.prs_full_frame_always
        ? std::vector<TimingCandidate>{}
        : find_full_frame_prs(samples, base_null_start_sample, config);
    if (cancelled()) return {};
    // The full-frame search identifies a PRS phase but does not retain its
    // channel response. Run the same local refinement so every usable PRS
    // candidate has comparable CIR diagnostics and window hypotheses.
    refine_prs(samples, config, full_frame);
    if (cancelled()) return {};
    for (const auto& prs : full_frame) {
        auto match = std::find_if(separated.begin(), separated.end(),
            [&](const auto& candidate) {
                return std::llabs(candidate.prs_refined_offset_samples -
                                  prs.prs_refined_offset_samples) <=
                    std::int64_t(config.prs_fine_step_samples * 2);
            });
        if (match == separated.end()) {
            separated.push_back(prs);
        } else if (prs.prs_metric > match->prs_metric) {
            const auto cp_metric = match->cp_metric;
            const auto null_depth = match->null_depth_db;
            const auto dominant = match->dominant_cluster;
            *match = prs;
            match->cp_metric = cp_metric;
            match->null_depth_db = null_depth;
            match->dominant_cluster = dominant;
        }
    }
    std::sort(separated.begin(), separated.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.prs_confirmed != rhs.prs_confirmed)
            return lhs.prs_confirmed > rhs.prs_confirmed;
        if (lhs.prs_metric != rhs.prs_metric)
            return lhs.prs_metric > rhs.prs_metric;
        return lhs.cp_metric > rhs.cp_metric;
    });
    if (separated.size() > config.max_candidates)
        separated.resize(config.max_candidates);
    return separated;
}

std::size_t refined_frame_phase_sample(const TimingCandidate& candidate)
{
    constexpr std::int64_t frame = 196608;
    const auto refinement = candidate.prs_refined_offset_samples -
        candidate.offset_samples;
    return std::size_t(positive_mod(
        std::int64_t(candidate.frame_phase_sample) + refinement, frame));
}

std::vector<ModeIFrameQuality> rank_mode_i_frames(
    const std::vector<std::complex<float>>& samples,
    std::size_t frame_phase_sample, std::size_t max_frames,
    const std::function<bool()>& cancel_requested)
{
    constexpr std::size_t frame = 196608;
    constexpr std::size_t symbols_per_frame = 76;
    std::vector<ModeIFrameQuality> result;
    if (max_frames == 0 || samples.size() < frame) return result;

    const auto phase = frame_phase_sample % frame;
    for (std::size_t frame_start = phase;
         frame_start + frame <= samples.size(); frame_start += frame) {
        if (cancel_requested && cancel_requested()) return {};
        std::vector<double> correlations;
        correlations.reserve(symbols_per_frame);
        double null_power = 0.0;
        for (std::size_t index = 0; index < null_length; ++index)
            null_power += std::norm(samples[frame_start + index]);
        null_power /= null_length;

        double symbol_power = 0.0;
        std::size_t symbol_power_samples = 0;
        for (std::size_t index = 0; index < symbols_per_frame; ++index) {
            const auto cp_start = frame_start + null_length + index * symbol;
            std::complex<double> correlation;
            double energy = 0.0;
            for (std::size_t sample = 0; sample < guard; ++sample) {
                const auto lhs = std::complex<double>(
                    samples[cp_start + sample]);
                const auto rhs = std::complex<double>(
                    samples[cp_start + useful + sample]);
                correlation += std::conj(lhs) * rhs;
                energy += std::norm(lhs) + std::norm(rhs);
            }
            correlations.push_back(
                energy > 0.0 ? 2.0 * std::abs(correlation) / energy : 0.0);

            // A sparse power estimate is sufficient for null contrast and
            // avoids another full pass over every useful symbol.
            for (std::size_t sample = 0; sample < useful; sample += 16) {
                symbol_power += std::norm(
                    samples[cp_start + guard + sample]);
                ++symbol_power_samples;
            }
        }
        symbol_power /= std::max<std::size_t>(1, symbol_power_samples);
        ModeIFrameQuality quality;
        quality.frame_start_sample = frame_start;
        quality.cp_metric = median(std::move(correlations));
        quality.null_depth_db = 10.0 * std::log10(
            std::max(symbol_power, 1e-20) / std::max(null_power, 1e-20));
        quality.rank = quality.cp_metric *
            std::max(0.05, quality.null_depth_db);
        result.push_back(quality);
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs,
                                               const auto& rhs) {
        if (lhs.rank != rhs.rank) return lhs.rank > rhs.rank;
        return lhs.frame_start_sample < rhs.frame_start_sample;
    });
    if (result.size() > max_frames) result.resize(max_frames);
    return result;
}

} // namespace dab
