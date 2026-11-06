#include "decoder_backend.hpp"
#include "libdxdabsdr.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace dab {
namespace {

class LibDxDabSdrBackend final : public DecoderBackend {
public:
    explicit LibDxDabSdrBackend(bool weighted_phase_plane)
    {
        settings_.weighted_phase_plane = weighted_phase_plane;
        settings_.adaptive_phase_plane = true;
        settings_.adaptive_fft_window = true;
        settings_.cancel_requested = [this] {
            return cancellation_requested();
        };
    }

    const char* name() const override { return "libdxdabsdr"; }
    bool supports_tii() const override { return true; }

    EnsembleInfo decode_ensemble(
        const std::vector<std::complex<float>>& samples,
        std::uint32_t frequency_khz) override
    {
        return dx::decode_fic(samples, frequency_khz, settings_).ensemble;
    }

    CandidateFicResult decode_ensemble_at_frame_phase(
        const std::vector<std::complex<float>>& samples,
        std::uint32_t frequency_khz, std::size_t frame_phase_sample,
        std::size_t max_frames, int integer_carrier_offset,
        bool spectrum_inverted) override
    {
        CandidateFicResult result;
        result.attempted = true;
        result.frame_phase_sample = frame_phase_sample;
        const auto decoded = dx::decode_fic_at_frame_phase(
            samples, frequency_khz, frame_phase_sample, max_frames,
            integer_carrier_offset, spectrum_inverted, settings_);
        result.projected_frames = decoded.stats.frames_examined;
        result.available_frames = decoded.stats.frames_available;
        result.temporal_ranked = decoded.stats.temporal_ranked;
        result.selected_first_sample = decoded.stats.selected_first_sample;
        result.selected_last_sample = decoded.stats.selected_last_sample;
        result.best_frame_cp_metric = decoded.stats.best_frame_cp_metric;
        result.best_frame_null_depth_db =
            decoded.stats.best_frame_null_depth_db;
        result.ensemble = decoded.ensemble;
        return result;
    }

    std::vector<EnsembleSubscan> decode_ensemble_subscans(
        const std::vector<std::complex<float>>& samples,
        std::uint32_t frequency_khz, std::size_t window_samples,
        std::size_t passes) override
    {
        std::vector<EnsembleSubscan> results;
        if (passes == 0 || window_samples < 196608 ||
            samples.size() < window_samples)
            return results;
        const auto last_start = samples.size() - window_samples;
        for (std::size_t pass = 0; pass < passes; ++pass) {
            if (cancellation_requested()) break;
            const auto start = passes == 1 ? last_start / 2
                : last_start * pass / (passes - 1);
            std::vector<std::complex<float>> window(
                samples.begin() + start,
                samples.begin() + start + window_samples);
            auto ensemble = dx::decode_fic(
                window, frequency_khz, settings_).ensemble;
            if (!ensemble.decoded) continue;
            const auto duplicate = std::find_if(
                results.begin(), results.end(), [&](const auto& prior) {
                    return prior.ensemble.ueid == ensemble.ueid;
                });
            if (duplicate == results.end())
                results.push_back({std::move(ensemble), start, window_samples});
        }
        return results;
    }

    TiiInfo decode_tii(const std::vector<std::complex<float>>& samples,
                       std::uint32_t frequency_khz,
                       const TiiDecodeSettings& settings) override
    {
        return dx::decode_tii(samples, frequency_khz, settings);
    }

private:
    dx::FicDecodeSettings settings_;
};

std::string normalized(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return char(std::tolower(c)); });
    return value;
}

} // namespace

std::unique_ptr<DecoderBackend> make_decoder_backend(
    const std::string& name)
{
    const auto selected = normalized(name);

    if (selected == "libdxdabsdr")
        return std::make_unique<LibDxDabSdrBackend>(false);
    throw std::runtime_error(
        "unknown decoder backend '" + name +
        "'; available backends: libdxdabsdr");
}

} // namespace dab
