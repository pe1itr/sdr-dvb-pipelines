#pragma once

#include <complex>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dab {

struct MuxSubchannelInfo {
    std::uint8_t id = 0;
    std::uint16_t start_address = 0;
    bool short_form = false;
    std::uint8_t table_index = 0;
    std::uint8_t protection_option = 0;
    std::uint8_t protection_level = 0;
    std::uint16_t size_cu = 0;
    std::optional<std::uint16_t> bitrate_kbps;
};

struct MuxServiceComponentInfo {
    std::uint8_t index = 0;
    std::uint8_t transport_mode = 0;
    std::uint8_t component_type = 0;
    std::optional<std::uint8_t> subchannel_id;
    std::optional<std::uint16_t> packet_component_id;
    bool primary = false;
    bool conditional_access = false;
};

struct MuxServiceInfo {
    std::uint32_t service_id = 0;
    bool programme = true;
    std::string label;
    std::vector<MuxServiceComponentInfo> components;
};

struct MuxMetadata {
    std::vector<MuxSubchannelInfo> subchannels;
    std::vector<MuxServiceInfo> services;
};

// Decoder-side differential-QPSK diagnostics. These are deliberately kept
// separate from the RF detector's CP-derived SNR: they describe the three FIC
// OFDM symbols after differential demodulation and residual phase correction.
struct DqpskQuality {
    bool available = false;
    bool phase_plane_enabled = false;
    bool phase_plane_attempted = false;
    bool legacy_fallback_used = false;
    bool adaptive_phase_plane = false;
    bool adaptive_fft_window = false;
    std::size_t intercept_frames = 0;
    std::size_t weighted_frames = 0;
    std::size_t fft_window_adjusted_frames = 0;
    double mean_fft_window_offset_samples = 0.0;
    std::size_t symbols = 0;
    double mean_coherence = std::numeric_limits<double>::quiet_NaN();
    double mean_evm_rms = std::numeric_limits<double>::quiet_NaN();
    double mean_mer_db = std::numeric_limits<double>::quiet_NaN();
    double mean_phase_intercept_rad = std::numeric_limits<double>::quiet_NaN();
    double mean_phase_slope_rad_per_carrier =
        std::numeric_limits<double>::quiet_NaN();
};

// Records which FIC demodulation path supplied the protocol evidence. The
// non-iterative 2D path is deliberately a last fallback: normal two-symbol
// differential decoding, adaptive phase correction and FFT-window selection
// are always exhausted first. Keeping attempted and selected separate makes a
// zero-result experiment visible without mislabelling ordinary detections.
struct FicDemodulationInfo {
    std::string selected_route = "TWO_SYMBOL";
    bool two_dimensional_attempted = false;
    bool two_dimensional_succeeded = false;
    std::size_t two_dimensional_carriers_per_block = 8;
    std::size_t two_dimensional_observations_per_carrier = 4;
    std::size_t two_dimensional_frames_examined = 0;
    std::size_t two_dimensional_codewords_decoded = 0;
    std::size_t two_dimensional_valid_fibs = 0;
    double two_dimensional_ms = 0.0;
};

// Protocol evidence extracted exclusively from CRC-valid FIBs. Keep this on
// an otherwise incomplete decode result: weak captures can then contribute a
// trustworthy EId, ECC or ensemble label to a later scan without retaining
// or copying raw IQ/FIB payloads.
struct FicIdentityEvidence {
    bool available = false;
    std::size_t frames_examined = 0;
    std::size_t codewords_decoded = 0;
    std::size_t valid_fibs = 0;
    std::size_t parsed_figs = 0;
    std::map<std::uint16_t, std::size_t> eid_counts;
    std::map<std::uint8_t, std::size_t> ecc_counts;
    std::map<std::uint16_t, std::map<std::string, std::size_t>> label_counts;
    // Key is (FIG type << 8) | extension.
    std::map<std::uint16_t, std::size_t> fig_counts;
};

struct EnsembleInfo {
    // True means an unambiguous non-zero ECC plus EId was recovered from the
    // FIC. The human-readable FIG 1/0 label is useful enrichment, not a gate.
    bool decoded = false;
    std::uint32_t ueid = 0;
    std::string label;
    double fic_snr_db = std::numeric_limits<double>::quiet_NaN();
    FicDemodulationInfo demodulation;
    DqpskQuality dqpsk;
    FicIdentityEvidence fic;
    MuxMetadata metadata;
};

struct EnsembleSubscan {
    EnsembleInfo ensemble;
    std::size_t start_sample = 0;
    std::size_t sample_count = 0;
};

struct CandidateFicResult {
    bool attempted = false;
    std::size_t candidate_index = 0;
    std::size_t frame_phase_sample = 0;
    std::size_t projected_frames = 0;
    std::size_t available_frames = 0;
    bool temporal_ranked = false;
    std::size_t selected_first_sample = 0;
    std::size_t selected_last_sample = 0;
    double best_frame_cp_metric = 0.0;
    double best_frame_null_depth_db = 0.0;
    std::string window_strategy = "normal";
    std::int64_t window_offset_samples = 0;
    EnsembleInfo ensemble;
    std::string rejection_reason;
};

struct TiiCandidate {
    std::uint8_t main_id = 0;
    std::uint8_t sub_id = 0;
    double level = 0.0;
    std::size_t hits = 0;
    double confidence = 0.0;
    std::size_t subdwell_hits = 0;
    std::size_t subdwell_observations = 0;
    bool confirmed = false;
    // A candidate may satisfy the temporal confirmation threshold while the
    // measured carrier groups still cannot distinguish it from another Main
    // ID on the same Sub ID. Keep it visible for diagnostics, but never treat
    // it as a transmitter observation.
    bool ambiguous = false;
};

struct TiiCollision {
    std::uint8_t sub_id = 0;
    std::uint8_t active_mask = 0;
    std::uint8_t active_blocks = 0;
    std::size_t hits = 0;
    double confidence = 0.0;
    std::vector<std::uint8_t> possible_main_ids;
};

struct TiiInfo {
    bool attempted = false;
    bool conservative = true;
    double dwell_ms = 0.0;
    std::size_t notifications = 0;
    std::size_t subdwells_attempted = 0;
    std::size_t required_subdwells = 0;
    std::vector<TiiCandidate> candidates;
    std::vector<TiiCollision> collisions;
};

// Effective thresholds for the independent DX TII decoder. The monitor
// resolves profile defaults and optional configuration overrides before each
// call; the production libdabsdr adapter uses only conservative/min_hits.
struct TiiDecodeSettings {
    bool conservative = true;
    std::size_t min_hits = 2;
    double min_pattern_quality = 0.30;
    double min_phase_coherence = 0.78;
    double min_pattern_margin = 0.05;
    double min_cell_level_ratio = 2.2;
    double collision_noise_level_ratio = 4.0;
    double collision_relative_threshold = 0.03;
    double min_confirmed_confidence = 0.50;
    double candidate_min_confidence = 0.10;
    double adjacent_max_relative_level = 0.03;
    double adjacent_min_dominance = 20.0;
    double subdwell_ms = 2000.0;
    std::size_t min_confirmed_subdwells = 3;
};

inline TiiDecodeSettings default_tii_decode_settings(
    bool conservative, std::size_t min_hits)
{
    TiiDecodeSettings settings;
    settings.conservative = conservative;
    settings.min_hits = min_hits;
    if (!conservative) {
        settings.min_pattern_quality = 0.20;
        settings.min_phase_coherence = 0.55;
        settings.min_pattern_margin = 0.03;
        settings.min_cell_level_ratio = 1.6;
        settings.min_confirmed_confidence = 0.0;
        settings.candidate_min_confidence = 0.0;
        settings.adjacent_max_relative_level = 0.0;
        settings.adjacent_min_dominance = 0.0;
    }
    return settings;
}

// Decode the FIC from 2.048 MS/s complex IQ. The implementation uses the
// separately distributed MIT-licensed AbracaDABra libdabsdr binary.
EnsembleInfo decode_ensemble(const std::vector<std::complex<float>>& samples,
                             std::uint32_t frequency_khz);

// Project raw Mode-I samples onto one PRS-confirmed frame phase before using
// the existing CRC-valid FIC decoder. Each OFDM useful window is selected on
// that timing raster; its cyclic prefix and the transmission-frame null are
// rebuilt. No identity is returned unless the normal FIC decoder validates it.
CandidateFicResult decode_ensemble_at_frame_phase(
    const std::vector<std::complex<float>>& samples,
    std::uint32_t frequency_khz, std::size_t frame_phase_sample,
    std::size_t max_frames);

// Re-run the FIC decoder on evenly distributed, overlapping time windows from
// one IQ capture. This does not retune or synthesize samples. It can expose a
// different ensemble when dominance changes during the capture; the decoder's
// CRC-valid FIC remains the identity gate. Results are unique by UEId.
std::vector<EnsembleSubscan> decode_ensemble_subscans(
    const std::vector<std::complex<float>>& samples,
    std::uint32_t frequency_khz, std::size_t window_samples,
    std::size_t passes);

// Decode transmitter identification information from a separate dwell on an
// already FIC-confirmed mux. Confidence is the fraction of TII notifications
// in which a Main/Sub pair occurred; min_hits controls confirmation.
TiiInfo decode_tii(const std::vector<std::complex<float>>& samples,
                   std::uint32_t frequency_khz, bool conservative,
                   std::size_t min_hits);

} // namespace dab
