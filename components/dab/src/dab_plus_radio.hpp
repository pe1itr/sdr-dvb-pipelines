#pragma once

#include "dynamic_label.hpp"
#include "mot_slideshow.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dab {

struct DabPlusService {
    std::uint32_t service_id = 0;
    std::uint8_t component_id = 0;
    std::string label;
    std::uint16_t bitrate_kbps = 0;
    bool primary = false;
};

// Queue peaks and loss counters cover this session (reset on channel change).
// Audio queue counts encoded access units, not PCM samples or playback latency.
struct DabPlusRadioMetrics {
    std::size_t iq_samples = 0, iq_peak_samples = 0, iq_capacity_samples = 0;
    std::size_t audio_frames = 0, audio_peak_frames = 0, audio_capacity_frames = 0;
    std::size_t dropped_iq = 0, dropped_audio = 0, alsa_underruns = 0;
    // Candidate windows, access units and concealments; not PCM sample counts.
    std::size_t rejected_superframe_candidates = 0, damaged_audio_units = 0;
    std::size_t aac_decode_errors = 0, concealed_audio_frames = 0;
};

// Build the MPEG-4 AudioSpecificConfig required for one DAB+ access unit.
// audio_header_raw follows ETSI TS 102 563 V2.1.1 table 2.
std::vector<std::uint8_t> dab_plus_audio_specific_config(
    std::uint8_t audio_header_raw);

// A persistent independent DX MSC session for interactive, single-channel DAB+
// listening. It deliberately remains separate from the block-oriented DX
// decoder API: dropping radio audio must never change DX evidence or logging.
class DabPlusRadioSession {
public:
    DabPlusRadioSession(std::string station_selector,
                        std::string alsa_device = "default",
                        bool playback_enabled = true);
    ~DabPlusRadioSession();

    DabPlusRadioSession(const DabPlusRadioSession&) = delete;
    DabPlusRadioSession& operator=(const DabPlusRadioSession&) = delete;

    void start(std::uint32_t frequency_khz);
    void stop() noexcept;
    void finish(); // Drain queued IQ at EOF; stop() remains immediate cancellation.
    void set_muted(bool muted);
    void select_station(const std::string& station_selector);
    void push_samples(const std::complex<float>* samples, std::size_t count);
    void check_error() const;

    std::vector<DabPlusService> services() const;
    std::size_t services_generation() const;
    bool dab_signal_synced() const;
    bool fic_synced() const;
    bool service_list_received() const;
    std::string ensemble_label() const;
    bool discovery_complete() const;
    bool station_available() const;
    bool station_selected() const;
    std::string selected_station() const;
    std::size_t dropped_iq_blocks() const;
    std::size_t dropped_audio_frames() const;
    DabPlusRadioMetrics metrics() const;
    std::size_t decoded_audio_frames() const;
    std::size_t decoded_slides() const;
    std::optional<MotSlide> take_slide();
    std::optional<DynamicLabel> take_dynamic_label();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dab
