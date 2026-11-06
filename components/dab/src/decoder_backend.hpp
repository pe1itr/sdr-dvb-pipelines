#pragma once

#include "dab_ensemble.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dab {

// Stable boundary between scanner/scheduler code and a DAB decoder library.
// A backend owns the complete FIC/TII decoding policy; the RF source, timing
// diagnostics, state tracking and outputs remain independent.
class DecoderBackend {
public:
    virtual ~DecoderBackend() = default;

    virtual const char* name() const = 0;
    virtual bool supports_tii() const = 0;
    virtual EnsembleInfo decode_ensemble(
        const std::vector<std::complex<float>>& samples,
        std::uint32_t frequency_khz) = 0;
    virtual CandidateFicResult decode_ensemble_at_frame_phase(
        const std::vector<std::complex<float>>& samples,
        std::uint32_t frequency_khz, std::size_t frame_phase_sample,
        std::size_t max_frames, int integer_carrier_offset,
        bool spectrum_inverted) = 0;
    virtual std::vector<EnsembleSubscan> decode_ensemble_subscans(
        const std::vector<std::complex<float>>& samples,
        std::uint32_t frequency_khz, std::size_t window_samples,
        std::size_t passes) = 0;
    virtual TiiInfo decode_tii(
        const std::vector<std::complex<float>>& samples,
        std::uint32_t frequency_khz,
        const TiiDecodeSettings& settings) = 0;

    void set_cancel_callback(std::function<bool()> callback)
    {
        cancel_callback_ = std::move(callback);
    }

    bool cancellation_requested() const
    {
        return cancel_callback_ && cancel_callback_();
    }

private:
    std::function<bool()> cancel_callback_;
};

// Construct a compiled-in backend. Unknown or not-yet-built names fail
// explicitly; unattended monitoring must never silently change decoder.
std::unique_ptr<DecoderBackend> make_decoder_backend(
    const std::string& name);

} // namespace dab
