#pragma once

#include "dab_detector.hpp"
#include "decoder_backend.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace dab {

struct CandidateFicAttempt {
    TimingCandidate timing;
    std::size_t refined_frame_phase_sample = 0;
    CandidateFicResult decode;
};

struct CaptureMuxResult {
    EnsembleInfo ensemble;
    std::optional<TimingCandidate> timing;
    std::size_t refined_frame_phase_sample = 0;
    bool accumulated = false;
    std::size_t accumulator_cluster_id = 0;
    std::size_t accumulator_fragments = 0;
    bool temporal_subscan = false;
    std::size_t subscan_start_sample = 0;
    std::size_t subscan_sample_count = 0;
};

struct CaptureDecodeResult {
    bool normal_attempted = false;
    bool normal_succeeded = false;
    std::optional<EnsembleInfo> normal_result;
    bool candidate_search_attempted = false;
    std::vector<CandidateFicAttempt> candidate_attempts;
    bool temporal_subscan_attempted = false;
    std::size_t temporal_subscan_window_samples = 0;
    std::size_t temporal_subscan_passes = 0;
    std::vector<EnsembleSubscan> temporal_subscans;
    std::vector<CaptureMuxResult> muxes;
};

// Decode all PRS-confirmed hypotheses and retain unique identities. Duplicate
// UEIDs keep the result backed by the highest PRS metric.
CaptureDecodeResult decode_prs_timing_candidates(
    const std::vector<std::complex<float>>& samples,
    std::uint32_t frequency_khz,
    const std::vector<TimingCandidate>& candidates,
    std::size_t max_frames, DecoderBackend& decoder);

// Primary capture policy: normal decode first and candidate search only after
// normal failure (including when phase_sync was never obtained). Optional
// temporal subscans are the final direct-decode fallback, after candidate FIC
// and before the caller considers accumulated partial evidence.
CaptureDecodeResult decode_capture_with_candidate_fallback(
    const std::vector<std::complex<float>>& samples,
    std::uint32_t frequency_khz, const Detection& detection,
    DecoderBackend& decoder,
    const TimingSearchConfig& timing_config = {},
    std::size_t max_candidate_frames = 10,
    std::size_t temporal_subscan_window_samples = 0,
    std::size_t temporal_subscan_passes = 0);

} // namespace dab
