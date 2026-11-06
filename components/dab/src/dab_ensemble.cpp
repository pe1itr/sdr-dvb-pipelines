#include "dab_ensemble.hpp"
#include "dab_detector.hpp"

#include <dabsdr.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <stdexcept>
#include <thread>

namespace dab {
namespace {

struct DecoderContext {
    const std::vector<std::complex<float>>* samples = nullptr;
    std::atomic<std::size_t> position{0};
    std::atomic<bool> exhausted{false};
    std::atomic<bool> fic_synced{false};
    std::atomic<bool> ensemble_requested{false};
    std::atomic<bool> decoded{false};
    std::mutex mutex;
    std::condition_variable changed;
    EnsembleInfo result;
    dabsdrHandle_t handle = nullptr;
    struct TiiStats {
        std::size_t hits = 0;
        double level_sum = 0.0;
    };
    bool tii_enabled = false;
    std::size_t tii_notifications = 0;
    std::map<std::pair<std::uint8_t, std::uint8_t>, TiiStats> tii_stats;
};

std::mutex decoder_mutex;
DecoderContext* active = nullptr;

void input_samples(float output[], std::uint16_t count)
{
    DecoderContext* context = active;
    if (!context || !context->samples) {
        std::fill(output, output + 2 * count, 0.0f);
        return;
    }
    std::size_t start = context->position.fetch_add(count);
    bool exhausted = start >= context->samples->size();
    for (std::size_t i = 0; i < count; ++i) {
        if (start + i < context->samples->size()) {
            // AbracaDABra's S16 input path presents raw ADC counts as floats.
            float in_phase = 32768.0f * (*context->samples)[start + i].real();
            float quadrature = 32768.0f * (*context->samples)[start + i].imag();
            output[2 * i] = in_phase;
            output[2 * i + 1] = quadrature;
        } else {
            output[2 * i] = 0.0f;
            output[2 * i + 1] = 0.0f;
        }
    }
    if (exhausted) {
        context->exhausted = true;
        context->changed.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void skip_samples(float[], std::uint16_t count)
{
    if (active) active->position.fetch_add(count);
}

void notification(dabsdrNotificationCBData_t* notification, void* opaque)
{
    auto* context = static_cast<DecoderContext*>(opaque);
    if (!context) return;
    if (notification->status != DABSDR_NSTAT_SUCCESS) {
        if (notification->nid == DABSDR_NID_ENSEMBLE_INFO) {
            context->ensemble_requested = false;
            context->changed.notify_all();
        }
        return;
    }
    if (notification->nid == DABSDR_NID_SYNC_STATUS) {
        auto* sync = static_cast<const dabsdrNtfSyncStatus_t*>(notification->pData);
        if (sync && sync->syncLevel == DABSDR_SYNC_LEVEL_FIC) {
            context->fic_synced = true;
            {
                std::lock_guard<std::mutex> lock(context->mutex);
                context->result.fic_snr_db = sync->snr10 / 10.0;
            }
            context->changed.notify_all();
        }
    } else if (notification->nid == DABSDR_NID_ENSEMBLE_INFO) {
        auto* ensemble = static_cast<const dabsdrNtfEnsemble_t*>(notification->pData);
        if (!ensemble || ensemble->ecc == 0) {
            // ECC 00 is not a usable over-the-air identity and may occur in an
            // early response before the real extended country code arrives.
            context->ensemble_requested = false;
            context->changed.notify_all();
            return;
        }
        EnsembleInfo result;
        result.decoded = true;
        result.ueid = ensemble->ueid;
        result.label.assign(ensemble->label.str,
            strnlen(ensemble->label.str, DAB_LABEL_MAX_LENGTH));
        while (!result.label.empty() && result.label.back() == ' ') result.label.pop_back();
        const bool has_label = !result.label.empty();
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            result.fic_snr_db = context->result.fic_snr_db;
            context->result = std::move(result);
        }
        if (has_label)
            context->decoded = true;
        else
            context->ensemble_requested = false;
        context->changed.notify_all();
    } else if (notification->nid == DABSDR_NID_TII && context->tii_enabled) {
        auto* tii = static_cast<const dabsdrNtfTii_t*>(notification->pData);
        if (!tii) return;
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            ++context->tii_notifications;
            const std::size_t count = std::min<std::size_t>(tii->numIds, 24);
            for (std::size_t i = 0; i < count; ++i) {
                auto& stats = context->tii_stats[{tii->id[i].main, tii->id[i].sub}];
                ++stats.hits;
                stats.level_sum += tii->id[i].level;
            }
        }
        context->changed.notify_all();
    }
}

} // namespace

EnsembleInfo decode_ensemble(const std::vector<std::complex<float>>& samples,
                             std::uint32_t frequency_khz)
{
    std::lock_guard<std::mutex> one_at_a_time(decoder_mutex);
    if (samples.size() < 196608) return {};

    DecoderContext context;
    context.samples = &samples;
    if (dabsdrInit(&context.handle) != EXIT_SUCCESS || !context.handle)
        throw std::runtime_error("libdabsdr initialization failed");

    active = &context;
    dabsdrRegisterInputFcn(context.handle, input_samples);
    dabsdrRegisterDummyInputFcn(context.handle, skip_samples);
    dabsdrRegisterNotificationCb(context.handle, notification, &context);
    dabsdr(context.handle);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    dabsdrRequest_Tune(context.handle, frequency_khz);

    // libdabsdr reports FIC sync before all FIGs are necessarily available.
    // Keep polling for a label, but retain ECC+EId as a valid identity if the
    // capture ends before FIG 1/0 arrives.
    const double input_seconds = samples.size() / 2048000.0;
    const auto timeout = std::chrono::milliseconds(std::llround(
        1000.0 * std::clamp(input_seconds + 2.0, 4.0, 35.0)));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto eof_deadline = deadline;
    auto next_request = std::chrono::steady_clock::now();
    bool eof_seen = false;
    while (!context.decoded.load() && std::chrono::steady_clock::now() < deadline) {
        if (context.exhausted.load() && !eof_seen) {
            eof_seen = true;
            eof_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        if (eof_seen && std::chrono::steady_clock::now() >= eof_deadline) break;

        const auto now = std::chrono::steady_clock::now();
        if (context.fic_synced.load() && now >= next_request &&
            !context.ensemble_requested.exchange(true)) {
            dabsdrRequest_GetEnsemble(context.handle);
            next_request = now + std::chrono::milliseconds(100);
        }

        std::unique_lock<std::mutex> lock(context.mutex);
        context.changed.wait_for(lock, std::chrono::milliseconds(50), [&] {
            return context.decoded.load();
        });
    }

    dabsdrRequest_Exit(context.handle);
    dabsdrDeinit(&context.handle);
    active = nullptr;
    return context.result;
}

CandidateFicResult decode_ensemble_at_frame_phase(
    const std::vector<std::complex<float>>& samples,
    std::uint32_t frequency_khz, std::size_t frame_phase_sample,
    std::size_t max_frames)
{
    constexpr std::size_t frame = 196608;
    constexpr std::size_t null_length = 2656;
    constexpr std::size_t guard = 504;
    constexpr std::size_t useful = 2048;
    constexpr std::size_t symbol = guard + useful;
    constexpr std::size_t symbols_per_frame = 76;

    CandidateFicResult result;
    result.attempted = true;
    result.frame_phase_sample = frame_phase_sample % frame;
    if (max_frames == 0 || samples.size() < frame) return result;

    std::vector<std::complex<float>> projected;
    projected.reserve(std::min(max_frames, samples.size() / frame) * frame);
    const auto ranked = rank_mode_i_frames(
        samples, result.frame_phase_sample, max_frames);
    result.temporal_ranked = true;
    result.available_frames = result.frame_phase_sample + frame <= samples.size()
        ? 1 + (samples.size() - result.frame_phase_sample - frame) / frame : 0;
    if (!ranked.empty()) {
        result.best_frame_cp_metric = ranked.front().cp_metric;
        result.best_frame_null_depth_db = ranked.front().null_depth_db;
        const auto by_time = std::minmax_element(
            ranked.begin(), ranked.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.frame_start_sample < rhs.frame_start_sample;
            });
        result.selected_first_sample = by_time.first->frame_start_sample;
        result.selected_last_sample = by_time.second->frame_start_sample;
    }
    for (const auto& selected : ranked) {
        const auto source_frame = selected.frame_start_sample;
        projected.insert(projected.end(), null_length, {0.0f, 0.0f});
        for (std::size_t index = 0; index < symbols_per_frame; ++index) {
            const auto useful_start = source_frame + null_length + guard +
                index * symbol;
            // Rebuild the CP from the selected useful window. This makes the
            // requested FFT raster explicit without synthesizing payload data.
            projected.insert(projected.end(),
                samples.begin() + useful_start + useful - guard,
                samples.begin() + useful_start + useful);
            projected.insert(projected.end(),
                samples.begin() + useful_start,
                samples.begin() + useful_start + useful);
        }
        ++result.projected_frames;
    }
    if (result.projected_frames == 0) return result;
    result.ensemble = decode_ensemble(projected, frequency_khz);
    return result;
}

std::vector<EnsembleSubscan> decode_ensemble_subscans(
    const std::vector<std::complex<float>>& samples,
    std::uint32_t frequency_khz, std::size_t window_samples,
    std::size_t passes)
{
    std::vector<EnsembleSubscan> results;
    if (passes == 0 || window_samples < 196608 || samples.size() < window_samples)
        return results;

    const std::size_t last_start = samples.size() - window_samples;
    std::map<std::uint32_t, std::size_t> identities;
    for (std::size_t pass = 0; pass < passes; ++pass) {
        const std::size_t start = passes == 1 ? last_start / 2
            : (last_start * pass) / (passes - 1);
        std::vector<std::complex<float>> window(
            samples.begin() + start, samples.begin() + start + window_samples);
        auto ensemble = decode_ensemble(window, frequency_khz);
        if (!ensemble.decoded) continue;
        auto [it, inserted] = identities.emplace(ensemble.ueid, results.size());
        if (inserted) {
            results.push_back({std::move(ensemble), start, window_samples});
        } else if (ensemble.fic_snr_db > results[it->second].ensemble.fic_snr_db) {
            results[it->second] = {std::move(ensemble), start, window_samples};
        }
    }
    return results;
}

TiiInfo decode_tii(const std::vector<std::complex<float>>& samples,
                   std::uint32_t frequency_khz, bool conservative,
                   std::size_t min_hits)
{
    TiiInfo result;
    result.attempted = true;
    result.conservative = conservative;
    result.dwell_ms = 1000.0 * samples.size() / 2048000.0;
    if (samples.size() < 196608) return result;

    std::lock_guard<std::mutex> one_at_a_time(decoder_mutex);
    DecoderContext context;
    context.samples = &samples;
    context.tii_enabled = true;
    if (dabsdrInit(&context.handle) != EXIT_SUCCESS || !context.handle)
        throw std::runtime_error("libdabsdr initialization failed");

    active = &context;
    dabsdrRegisterInputFcn(context.handle, input_samples);
    dabsdrRegisterDummyInputFcn(context.handle, skip_samples);
    dabsdrRegisterNotificationCb(context.handle, notification, &context);
    dabsdr(context.handle);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    dabsdrRequest_Tune(context.handle, frequency_khz);
    dabsdrRequest_SetTII(context.handle, 1, conservative
        ? DABSDR_TII_MODE_CONSERVATIVE : DABSDR_TII_MODE_DEFAULT);

    const double input_seconds = samples.size() / 2048000.0;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::llround(1000.0 *
            std::clamp(input_seconds + 2.0, 4.0, 35.0)));
    bool eof_seen = false;
    auto eof_deadline = deadline;
    while (std::chrono::steady_clock::now() < deadline) {
        if (context.exhausted.load() && !eof_seen) {
            eof_seen = true;
            eof_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        if (eof_seen && std::chrono::steady_clock::now() >= eof_deadline) break;
        std::unique_lock<std::mutex> lock(context.mutex);
        context.changed.wait_for(lock, std::chrono::milliseconds(50));
    }

    dabsdrRequest_SetTII(context.handle, 0, DABSDR_TII_MODE_CONSERVATIVE);
    dabsdrRequest_Exit(context.handle);
    dabsdrDeinit(&context.handle);
    active = nullptr;

    result.notifications = context.tii_notifications;
    for (const auto& [id, stats] : context.tii_stats) {
        TiiCandidate candidate;
        candidate.main_id = id.first;
        candidate.sub_id = id.second;
        candidate.hits = stats.hits;
        candidate.level = stats.level_sum / stats.hits;
        candidate.confidence = result.notifications == 0 ? 0.0
            : double(stats.hits) / result.notifications;
        candidate.confirmed = stats.hits >= min_hits;
        result.candidates.push_back(candidate);
    }
    std::sort(result.candidates.begin(), result.candidates.end(),
        [](const TiiCandidate& lhs, const TiiCandidate& rhs) {
            if (lhs.confirmed != rhs.confirmed) return lhs.confirmed > rhs.confirmed;
            if (lhs.hits != rhs.hits) return lhs.hits > rhs.hits;
            return lhs.level > rhs.level;
        });
    return result;
}

} // namespace dab
