#pragma once

#include <complex>
#include <cstddef>
#include <functional>
#include <vector>

namespace dab {

// Lightweight DAB Mode-I channel filter for decoder input at 2.048 MS/s.
// It leaves the occupied +/-768 kHz carriers essentially flat and suppresses
// an adjacent-channel edge near +/-944 kHz. The centred FIR preserves sample
// indices used by timing diagnostics; only the short file boundaries see
// zero-padding transients.
std::vector<float> make_channel_filter_taps(double sample_rate);

double channel_filter_response_db(const std::vector<float>& taps,
                                  double sample_rate,
                                  double frequency_hz);

bool apply_channel_filter(
    std::vector<std::complex<float>>& samples, double sample_rate,
    const std::function<bool()>& cancel_requested = {});

// Filter a wider, integer-rate capture before reducing it to the fixed DAB
// Mode-I decoder rate. Filtering and decimation are combined so rejected
// adjacent-channel energy can never alias into the 2.048 MS/s result.
// The centred FIR keeps output sample zero aligned with input sample zero.
bool channelize_to_mode_i(
    std::vector<std::complex<float>>& samples, double input_sample_rate,
    double output_sample_rate = 2048000.0,
    const std::function<bool()>& cancel_requested = {});

// Stateful causal form of the same FIR for live diagnostics. Its magnitude
// response is identical to apply_channel_filter(); the output has the normal
// FIR group delay and is therefore intended for displays, not decoder timing.
class StreamingChannelFilter {
public:
    explicit StreamingChannelFilter(double sample_rate);

    void reset();
    void process(const std::complex<float>* samples, std::size_t count,
                 std::vector<std::complex<float>>& output);

private:
    std::vector<float> taps_;
    std::vector<std::complex<float>> history_;
    std::size_t position_ = 0;
};

// Causal streaming form of channelize_to_mode_i() for live consumers. It
// evaluates the symmetric FIR only at retained output instants, preventing a
// filtered diagnostic display from doing needless work on discarded samples.
class StreamingChannelizer {
public:
    StreamingChannelizer(double input_sample_rate,
                         double output_sample_rate = 2048000.0);

    void reset();
    void process(const std::complex<float>* samples, std::size_t count,
                 std::vector<std::complex<float>>& output);

private:
    std::vector<float> taps_;
    // Each ring element is mirrored one ring length later, allowing a full
    // reverse FIR window to be accessed without modulo operations.
    std::vector<std::complex<float>> history_;
    std::vector<float> paired_taps_;
    std::size_t ring_size_ = 0;
    std::size_t position_ = 0;
    std::size_t decimation_ = 1;
    std::size_t decimation_phase_ = 0;
};

} // namespace dab
