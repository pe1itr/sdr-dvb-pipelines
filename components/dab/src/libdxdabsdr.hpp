#pragma once

#include "dab_ensemble.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace dab::dx {

struct StreamFrame {
    EnsembleInfo ensemble;
    std::vector<float> msc; // Four CIFs, positive = bit 0, zero = erasure.
    bool fic_valid = false;
    bool discontinuity = false;
};

// Streaming extension of our independent FIC/TII demodulator. Its state is
// owned by one receive thread; reset on IQ loss or channel change.
class StreamDemodulator {
public:
    StreamDemodulator();
    ~StreamDemodulator();
    void reset();
    void feed(const std::complex<float>* iq, std::size_t count,
              const std::function<void(StreamFrame&&)>& output);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct FicDecodeSettings {
    FicDecodeSettings(bool weighted = true, bool adaptive_phase = false,
                      bool adaptive_window = false)
        : weighted_phase_plane(weighted),
          adaptive_phase_plane(adaptive_phase),
          adaptive_fft_window(adaptive_window)
    {}

    // Select the fixed phase model when adaptive_phase_plane is false.
    bool weighted_phase_plane = true;
    // Decode both intercept-only and weighted phase-plane hypotheses from the
    // same FFT/DQPSK values and retain the one with most CRC-good FIBs per
    // frame. Ties deliberately retain the intercept-only production baseline.
    bool adaptive_phase_plane = false;
    // Try earlier useful-symbol starts at 64-sample spacing across most of the
    // Mode-I guard interval. Selection is per frame and CRC-driven; offset zero
    // wins ties so a strong signal follows the former path exactly.
    bool adaptive_fft_window = false;
    // After every existing two-symbol/phase/window route has failed to recover
    // an identity, try a non-iterative 8-carrier by 4-observation detector.
    bool two_dimensional_fallback = true;
    // Diagnostic-only mode for reproducible A/B tests. Production backends do
    // not set this: it bypasses the established routes instead of following
    // the fallback policy above.
    bool two_dimensional_only = false;
    // Optional cooperative cancellation for an interactive channel retune.
    std::function<bool()> cancel_requested;
};

struct FicStats {
    std::size_t timing_candidates = 0;
    std::size_t frames_available = 0;
    std::size_t frames_examined = 0;
    bool temporal_ranked = false;
    std::size_t selected_first_sample = 0;
    std::size_t selected_last_sample = 0;
    double best_frame_cp_metric = 0.0;
    double best_frame_null_depth_db = 0.0;
    std::size_t codewords_decoded = 0;
    std::size_t valid_fibs = 0;
    FicDemodulationInfo demodulation;
    DqpskQuality dqpsk;
};

struct DecodeResult {
    EnsembleInfo ensemble;
    FicStats stats;
};

struct ConstellationPoint {
    std::complex<float> value;
    // Zero-based data-symbol index within a Mode-I frame: 0..2 are FIC,
    // 3..74 are MSC. PRS is the differential reference and is not a point.
    std::uint8_t data_symbol = 0;
};

struct ConstellationSnapshot {
    std::vector<ConstellationPoint> points;
    DqpskQuality quality;
    double strong_phase_coherence = 0.0;
    std::size_t strong_point_count = 0;
    std::size_t data_symbols = 0;
    std::size_t frame_start_sample = 0;
    double fine_offset_hz = 0.0;
    int integer_carrier_offset = 0;
    bool spectrum_inverted = false;
};

// Independent ETSI EN 300 401 Mode-I FIC decoder. It does not call
// libdabsdr. Input is complex float IQ at exactly 2.048 MS/s.
DecodeResult decode_fic(const std::vector<std::complex<float>>& samples,
                        std::uint32_t frequency_khz,
                        const FicDecodeSettings& settings = {});

// Decode with an explicitly selected Mode-I null/frame phase. This is used by
// the PRS candidate path; frame_phase_sample is modulo 196608 samples.
DecodeResult decode_fic_at_frame_phase(
    const std::vector<std::complex<float>>& samples,
    std::uint32_t frequency_khz, std::size_t frame_phase_sample,
    std::size_t max_frames, const FicDecodeSettings& settings = {});

DecodeResult decode_fic_at_frame_phase(
    const std::vector<std::complex<float>>& samples,
    std::uint32_t frequency_khz, std::size_t frame_phase_sample,
    std::size_t max_frames, int integer_carrier_offset,
    bool spectrum_inverted, const FicDecodeSettings& settings = {});

// Extract normalized, phase-corrected differential-QPSK points from all 75
// data symbols (3 FIC + 72 MSC) in the most recent complete Mode-I frame. MSC
// bits are not decoded. This diagnostic uses the same FFT/carrier/DQPSK path
// as the independent decoder and does not change decoder decisions or state.
ConstellationSnapshot extract_constellation(
    const std::vector<std::complex<float>>& samples,
    std::size_t max_points = 4608,
    const FicDecodeSettings& settings = {});

// Diagnostic variant with an explicit Mode-I frame phase and synchronizer
// hypotheses. It enables reproducible FFT-window A/B measurements without
// changing the normal detector or decoder selection.
ConstellationSnapshot extract_constellation_at_frame_phase(
    const std::vector<std::complex<float>>& samples,
    std::size_t frame_phase_sample, double fine_offset_hz,
    int integer_carrier_offset, bool spectrum_inverted,
    std::size_t max_points = 4608,
    const FicDecodeSettings& settings = {});

// Independent Mode-I TII decoder. It correlates the adjacent carrier pairs in
// active null symbols and aggregates Main/Sub observations across frames.
TiiInfo decode_tii(const std::vector<std::complex<float>>& samples,
                   std::uint32_t frequency_khz,
                   const TiiDecodeSettings& settings);

// Convenience overload for offline tools using the selected profile defaults.
inline TiiInfo decode_tii(const std::vector<std::complex<float>>& samples,
                          std::uint32_t frequency_khz, bool conservative,
                          std::size_t min_hits)
{
    return decode_tii(samples, frequency_khz,
                      default_tii_decode_settings(conservative, min_hits));
}

} // namespace dab::dx
