#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace dab {

struct DetectorConfig {
    double sample_rate = 2048000.0;
    double cp_threshold = 0.20;
    std::size_t min_cp_peaks = 20;
    double min_null_depth_db = 1.5;
};

struct Detection {
    bool energy_present = false;
    bool phase_sync = false;
    bool null_sync = false;
    std::size_t cp_peaks = 0;
    double rms_dbfs = -200.0;
    double cp_metric_median = 0.0;
    double cp_metric_max = 0.0;
    double estimated_snr_db = -200.0;
    double fine_offset_hz = 0.0;
    double lock_time_ms = 0.0;
    double null_depth_db = 0.0;
    std::size_t null_start_sample = 0;
};

struct TimingSearchConfig {
    TimingSearchConfig() = default;
    TimingSearchConfig(double sample_rate_, double max_delay_us_,
                       std::size_t coarse_step_samples_,
                       std::size_t fine_step_samples_,
                       std::size_t max_candidates_,
                       std::size_t min_separation_samples_,
                       std::size_t prs_refine_radius_samples_,
                       std::size_t prs_fine_step_samples_,
                       double prs_min_metric_,
                       double prs_full_frame_min_metric_,
                       std::size_t prs_max_frames_,
                       bool prs_full_frame_always_)
        : sample_rate(sample_rate_), max_delay_us(max_delay_us_),
          coarse_step_samples(coarse_step_samples_),
          fine_step_samples(fine_step_samples_),
          max_candidates(max_candidates_),
          min_separation_samples(min_separation_samples_),
          prs_refine_radius_samples(prs_refine_radius_samples_),
          prs_fine_step_samples(prs_fine_step_samples_),
          prs_min_metric(prs_min_metric_),
          prs_full_frame_min_metric(prs_full_frame_min_metric_),
          prs_max_frames(prs_max_frames_),
          prs_full_frame_always(prs_full_frame_always_)
    {}

    double sample_rate = 2048000.0;
    double max_delay_us = 2500.0;
    std::size_t coarse_step_samples = 16;
    std::size_t fine_step_samples = 2;
    std::size_t max_candidates = 6;
    std::size_t min_separation_samples = 504;
    std::size_t prs_refine_radius_samples = 504;
    std::size_t prs_fine_step_samples = 2;
    double prs_min_metric = 0.08;
    double prs_full_frame_min_metric = 0.20;
    std::size_t prs_max_frames = 10;
    bool prs_full_frame_always = true;
    double cir_peak_relative_threshold = 0.08;
    double cir_peak_noise_ratio = 6.0;
    std::size_t cir_peak_min_separation_samples = 8;
    std::size_t cir_max_peaks = 8;
    // Candidate-FIC fallback may rank PRS frames across a long capture. Keep
    // this opt-in so normal FIC/TII synchronizers retain their established
    // sequential PRS behavior.
    bool prs_temporal_rank = false;
    // Optional cooperative cancellation for interactive use. Expensive PRS
    // searches stop at bounded inner-loop checkpoints when this returns true.
    std::function<bool()> cancel_requested;
};

struct CirPeak {
    std::int64_t delay_samples = 0;
    double delay_us = 0.0;
    double relative_level = 0.0;
};

struct TimingCandidate {
    std::int64_t offset_samples = 0;
    double offset_us = 0.0;
    std::size_t frame_phase_sample = 0;
    double cp_metric = 0.0;
    double null_depth_db = 0.0;
    bool dominant_cluster = false;
    std::int64_t prs_refined_offset_samples = 0;
    double prs_refined_offset_us = 0.0;
    double prs_metric = 0.0;
    std::size_t prs_frames = 0;
    bool prs_confirmed = false;
    int prs_integer_carrier_offset = 0;
    bool prs_spectrum_inverted = false;
    bool cir_available = false;
    std::size_t cir_frames = 0;
    double cir_noise_relative_level = 0.0;
    std::vector<CirPeak> cir_peaks;
    std::int64_t cir_strongest_path_offset_samples = 0;
    std::int64_t cir_quasi_optimal_offset_samples = 0;
    std::int64_t cir_cluster_min_delay_samples = 0;
    std::int64_t cir_cluster_max_delay_samples = 0;
    std::int64_t cir_guard_overlap_samples = 0;
    bool cir_isi_free = false;
};

// Cheap temporal quality estimate for one complete Mode-I frame at an
// already-known frame phase. This deliberately performs no FFT or FIC decode;
// it is used to find short useful passages anywhere in a long RF capture.
struct ModeIFrameQuality {
    std::size_t frame_start_sample = 0;
    double cp_metric = 0.0;
    double null_depth_db = 0.0;
    double rank = 0.0;
};

Detection detect_mode_i(const std::vector<std::complex<float>>& samples,
                        const DetectorConfig& config);

// Score multiple Mode-I frame timing anchors around a known null start. The
// score combines cyclic-prefix correlation at all expected OFDM symbols and
// frames. It does not decode FIC and therefore never claims an ensemble ID.
std::vector<TimingCandidate> find_mode_i_timing_candidates(
    const std::vector<std::complex<float>>& samples,
    std::size_t base_null_start_sample, const TimingSearchConfig& config);

// Convert a coarse frame phase plus PRS refinement into the Mode-I frame
// phase used by candidate FIC decoders. This is the single shared definition
// for detector, monitor orchestration and independent decoder paths.
std::size_t refined_frame_phase_sample(const TimingCandidate& candidate);

// Rank every complete frame on the selected Mode-I raster. At most
// max_frames are returned, strongest first. A zero limit returns no frames.
std::vector<ModeIFrameQuality> rank_mode_i_frames(
    const std::vector<std::complex<float>>& samples,
    std::size_t frame_phase_sample, std::size_t max_frames,
    const std::function<bool()>& cancel_requested = {});

// Fixed Mode-I phase-reference useful symbol generated directly from ETSI
// EN 300 401 clause 14.3.2, tables 23 and 24. The 504-sample cyclic prefix is
// deliberately not included.
const std::vector<std::complex<float>>& mode_i_prs_reference();

} // namespace dab
