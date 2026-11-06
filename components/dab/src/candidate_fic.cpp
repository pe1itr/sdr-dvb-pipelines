#include "candidate_fic.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace dab {

CaptureDecodeResult decode_prs_timing_candidates(
    const std::vector<std::complex<float>>& samples,
    std::uint32_t frequency_khz,
    const std::vector<TimingCandidate>& candidates,
    std::size_t max_frames, DecoderBackend& decoder)
{
    CaptureDecodeResult result;
    result.candidate_search_attempted = true;
    std::map<std::uint32_t, std::size_t> identities;
    std::set<std::tuple<std::size_t, int, bool>> attempted_hypotheses;
    for (const auto& candidate : candidates) {
        if (decoder.cancellation_requested()) break;
        if (!candidate.prs_confirmed) continue;
        CandidateFicAttempt attempt;
        attempt.timing = candidate;
        attempt.refined_frame_phase_sample =
            refined_frame_phase_sample(candidate);
        if (!attempted_hypotheses.emplace(
                attempt.refined_frame_phase_sample,
                candidate.prs_integer_carrier_offset,
                candidate.prs_spectrum_inverted).second)
            continue;
        attempt.decode = decoder.decode_ensemble_at_frame_phase(
            samples, frequency_khz, attempt.refined_frame_phase_sample,
            max_frames, candidate.prs_integer_carrier_offset,
            candidate.prs_spectrum_inverted);
        result.candidate_attempts.push_back(std::move(attempt));
        const auto& stored = result.candidate_attempts.back();
        if (!stored.decode.ensemble.decoded) continue;

        const auto ueid = stored.decode.ensemble.ueid;
        auto [identity, inserted] = identities.emplace(ueid, result.muxes.size());
        CaptureMuxResult mux{
            stored.decode.ensemble, stored.timing,
            stored.refined_frame_phase_sample, false, 0, 0};
        if (inserted) {
            result.muxes.push_back(std::move(mux));
        } else if (stored.timing.prs_metric >
                   result.muxes[identity->second].timing->prs_metric) {
            result.muxes[identity->second] = std::move(mux);
        }
    }
    return result;
}

CaptureDecodeResult decode_capture_with_candidate_fallback(
    const std::vector<std::complex<float>>& samples,
    std::uint32_t frequency_khz, const Detection& detection,
    DecoderBackend& decoder, const TimingSearchConfig& timing_config,
    std::size_t max_candidate_frames,
    std::size_t temporal_subscan_window_samples,
    std::size_t temporal_subscan_passes)
{
    CaptureDecodeResult result;
    if (decoder.cancellation_requested()) return result;
    std::optional<EnsembleInfo> normal_result;
    if (detection.phase_sync) {
        result.normal_attempted = true;
        auto ensemble = decoder.decode_ensemble(samples, frequency_khz);
        if (decoder.cancellation_requested()) {
            result.normal_result = std::move(ensemble);
            return result;
        }
        if (ensemble.decoded) {
            result.normal_succeeded = true;
            result.normal_result = ensemble;
            result.muxes.push_back(
                {std::move(ensemble), std::nullopt, 0, false, 0, 0});
            return result;
        }
        normal_result = std::move(ensemble);
    }

    if (decoder.cancellation_requested()) {
        result.normal_result = std::move(normal_result);
        return result;
    }

    auto candidate_timing = timing_config;
    candidate_timing.prs_temporal_rank = true;
    candidate_timing.cancel_requested = [&decoder] {
        return decoder.cancellation_requested();
    };
    auto candidates = find_mode_i_timing_candidates(
        samples, detection.null_start_sample, candidate_timing);
    if (decoder.cancellation_requested()) {
        result.normal_result = std::move(normal_result);
        return result;
    }
    result = decode_prs_timing_candidates(
        samples, frequency_khz, candidates, max_candidate_frames, decoder);
    result.normal_attempted = detection.phase_sync;
    // Preserve CRC-valid protocol evidence from the primary route even when
    // it did not yet contain both ECC and EId.
    result.normal_result = std::move(normal_result);
    if (result.muxes.empty() && temporal_subscan_window_samples != 0 &&
        temporal_subscan_passes != 0 && !decoder.cancellation_requested()) {
        result.temporal_subscan_attempted = true;
        result.temporal_subscan_window_samples =
            temporal_subscan_window_samples;
        result.temporal_subscan_passes = temporal_subscan_passes;
        result.temporal_subscans = decoder.decode_ensemble_subscans(
            samples, frequency_khz, temporal_subscan_window_samples,
            temporal_subscan_passes);
        std::set<std::uint32_t> identities;
        for (const auto& subscan : result.temporal_subscans) {
            if (!subscan.ensemble.decoded ||
                !identities.insert(subscan.ensemble.ueid).second)
                continue;
            CaptureMuxResult mux{
                subscan.ensemble, std::nullopt, 0, false, 0, 0};
            mux.temporal_subscan = true;
            mux.subscan_start_sample = subscan.start_sample;
            mux.subscan_sample_count = subscan.sample_count;
            result.muxes.push_back(std::move(mux));
        }
    }
    return result;
}

} // namespace dab
