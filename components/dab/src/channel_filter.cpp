#include "channel_filter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace dab {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr std::size_t reference_tap_count = 81;
constexpr double reference_sample_rate = 2048000.0;
constexpr double passband_edge_hz = 768000.0;
constexpr double stopband_edge_hz = 850000.0;

} // namespace

std::vector<float> make_channel_filter_taps(double sample_rate)
{
    if (!(sample_rate > 2.0 * stopband_edge_hz))
        throw std::invalid_argument(
            "channel filter sample rate must exceed 1.7 MS/s");
    const double cutoff_hz =
        0.5 * (passband_edge_hz + stopband_edge_hz);
    // Preserve approximately the same impulse-response duration when the
    // input rate is widened. 81 taps at 2.048 MS/s becomes 161 at 4.096 MS/s.
    std::size_t tap_count = std::size_t(std::llround(
        (reference_tap_count - 1) * sample_rate / reference_sample_rate)) + 1;
    tap_count = std::max<std::size_t>(reference_tap_count, tap_count);
    if ((tap_count & 1U) == 0) ++tap_count;
    const int half = int(tap_count / 2);
    std::vector<float> taps(tap_count);
    double sum = 0.0;
    for (std::size_t index = 0; index < taps.size(); ++index) {
        const int offset = int(index) - half;
        const double ideal = offset == 0
            ? 2.0 * cutoff_hz / sample_rate
            : std::sin(2.0 * pi * cutoff_hz * offset / sample_rate) /
                (pi * offset);
        const double hamming = 0.54 - 0.46 * std::cos(
            2.0 * pi * double(index) / double(taps.size() - 1));
        taps[index] = float(ideal * hamming);
        sum += ideal * hamming;
    }
    for (auto& tap : taps) tap = float(tap / sum);
    return taps;
}

double channel_filter_response_db(const std::vector<float>& taps,
                                  double sample_rate,
                                  double frequency_hz)
{
    if (taps.empty() || !(sample_rate > 0.0))
        throw std::invalid_argument("invalid channel filter response input");
    std::complex<double> response{};
    for (std::size_t index = 0; index < taps.size(); ++index) {
        const double phase = -2.0 * pi * frequency_hz *
            double(index) / sample_rate;
        response += double(taps[index]) * std::polar(1.0, phase);
    }
    return 20.0 * std::log10(std::max(1e-15, std::abs(response)));
}

bool apply_channel_filter(
    std::vector<std::complex<float>>& samples, double sample_rate,
    const std::function<bool()>& cancel_requested)
{
    if (samples.empty()) return true;
    const auto taps = make_channel_filter_taps(sample_rate);
    const std::size_t half = taps.size() / 2;
    std::vector<std::complex<float>> filtered(samples.size());
    for (std::size_t output = 0; output < samples.size(); ++output) {
        if ((output & 0x3fffU) == 0 && cancel_requested &&
            cancel_requested())
            return false;
        std::complex<float> sum{};
        const std::size_t first_tap = output < half ? half - output : 0;
        const std::size_t last_tap = std::min(
            taps.size(), samples.size() + half - output);
        for (std::size_t tap = first_tap; tap < last_tap; ++tap)
            sum += samples[output + tap - half] * taps[tap];
        filtered[output] = sum;
    }
    samples.swap(filtered);
    return true;
}

bool channelize_to_mode_i(
    std::vector<std::complex<float>>& samples, double input_sample_rate,
    double output_sample_rate,
    const std::function<bool()>& cancel_requested)
{
    if (samples.empty()) return true;
    if (!(input_sample_rate > 0.0) || !(output_sample_rate > 0.0) ||
        input_sample_rate < output_sample_rate)
        throw std::invalid_argument("invalid channelizer sample rates");
    const double ratio = input_sample_rate / output_sample_rate;
    const auto decimation = std::size_t(std::llround(ratio));
    if (decimation == 0 ||
        std::abs(ratio - double(decimation)) > 1e-9)
        throw std::invalid_argument(
            "channelizer requires an integer input/output sample-rate ratio");
    if (decimation == 1) {
        return apply_channel_filter(
            samples, input_sample_rate, cancel_requested);
    }

    const auto taps = make_channel_filter_taps(input_sample_rate);
    const std::size_t half = taps.size() / 2;
    const std::size_t output_count = samples.size() / decimation;
    std::vector<std::complex<float>> output(output_count);
    for (std::size_t index = 0; index < output_count; ++index) {
        if ((index & 0x3fffU) == 0 && cancel_requested &&
            cancel_requested())
            return false;
        const std::size_t input = index * decimation;
        std::complex<float> sum{};
        const std::size_t first_tap = input < half ? half - input : 0;
        const std::size_t last_tap = std::min(
            taps.size(), samples.size() + half - input);
        for (std::size_t tap = first_tap; tap < last_tap; ++tap)
            sum += samples[input + tap - half] * taps[tap];
        output[index] = sum;
    }
    samples.swap(output);
    return true;
}

StreamingChannelFilter::StreamingChannelFilter(double sample_rate)
    : taps_(make_channel_filter_taps(sample_rate)), history_(taps_.size())
{
}

void StreamingChannelFilter::reset()
{
    std::fill(history_.begin(), history_.end(), std::complex<float>{});
    position_ = 0;
}

void StreamingChannelFilter::process(
    const std::complex<float>* samples, std::size_t count,
    std::vector<std::complex<float>>& output)
{
    if (!samples && count != 0)
        throw std::invalid_argument("null streaming channel filter input");
    output.resize(count);
    for (std::size_t sample = 0; sample < count; ++sample) {
        history_[position_] = samples[sample];
        std::complex<float> sum{};
        for (std::size_t tap = 0; tap < taps_.size(); ++tap) {
            const std::size_t history_index =
                (position_ + history_.size() - tap) % history_.size();
            sum += history_[history_index] * taps_[tap];
        }
        output[sample] = sum;
        position_ = (position_ + 1) % history_.size();
    }
}

StreamingChannelizer::StreamingChannelizer(double input_sample_rate,
                                           double output_sample_rate)
    : taps_(make_channel_filter_taps(input_sample_rate)),
      ring_size_(taps_.size())
{
    if (!(output_sample_rate > 0.0) ||
        input_sample_rate < output_sample_rate)
        throw std::invalid_argument("invalid streaming channelizer rates");
    const double ratio = input_sample_rate / output_sample_rate;
    decimation_ = std::size_t(std::llround(ratio));
    if (decimation_ == 0 ||
        std::abs(ratio - double(decimation_)) > 1e-9)
        throw std::invalid_argument(
            "streaming channelizer requires an integer sample-rate ratio");
    history_.assign(2 * ring_size_, {});
    for (std::size_t tap=0;tap+1<taps_.size()/2;tap+=2) {
        paired_taps_.insert(paired_taps_.end(),{taps_[tap+1],taps_[tap+1],taps_[tap],taps_[tap]});
    }
}

void StreamingChannelizer::reset()
{
    std::fill(history_.begin(), history_.end(), std::complex<float>{});
    position_ = 0;
    decimation_phase_ = 0;
}

void StreamingChannelizer::process(
    const std::complex<float>* samples, std::size_t count,
    std::vector<std::complex<float>>& output)
{
    if (!samples && count != 0)
        throw std::invalid_argument("null streaming channelizer input");
    output.clear();
    output.reserve((count + decimation_ - 1) / decimation_ + 1);
    const std::size_t half = taps_.size() / 2;
    for (std::size_t sample = 0; sample < count; ++sample) {
        history_[position_] = samples[sample];
        history_[position_ + ring_size_] = samples[sample];
        if (++position_ == ring_size_) position_=0;
        if (decimation_phase_ != 0) {
            if (++decimation_phase_ == decimation_) decimation_phase_=0;
            continue;
        }
        decimation_phase_ = decimation_==1 ? 0 : 1;

        const std::size_t newest = position_ + ring_size_ - 1;
        std::complex<float> sum =
            history_[newest - half] * taps_[half];
        std::size_t tap=0;
#if defined(__GNUC__) || defined(__clang__)
        // Process two symmetric complex pairs per SIMD operation. memcpy
        // permits unaligned ring windows without aliasing/alignment assumptions.
        // Keep exactly the original FIR; only its accumulation order changes.
        using Float4 = float __attribute__((vector_size(16)));
        Float4 accumulator={0,0,0,0};
        for (;tap+1<half;tap+=2) {
            Float4 front,back,weights;
            std::memcpy(&front,&history_[newest-tap-1],sizeof(front));
            std::memcpy(&back,&history_[newest-(ring_size_-1-tap)],sizeof(back));
            std::memcpy(&weights,&paired_taps_[2*tap],sizeof(weights));
            back=Float4{back[2],back[3],back[0],back[1]};
            accumulator+=(front+back)*weights;
        }
        sum+=std::complex<float>{accumulator[0]+accumulator[2],accumulator[1]+accumulator[3]};
#endif
        for (;tap<half;++tap)
            sum+=(history_[newest-tap]+history_[newest-(ring_size_-1-tap)])*taps_[tap];
        output.push_back(sum);
    }
}

} // namespace dab
