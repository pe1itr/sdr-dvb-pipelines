#include "fic_identity_accumulator.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace dab {
namespace {

template <typename Key>
std::optional<Key> unique_most_frequent(const std::map<Key, std::size_t>& values)
{
    std::optional<Key> selected;
    std::size_t best = 0;
    bool tied = false;
    for (const auto& [value, count] : values) {
        if (count > best) {
            selected = value;
            best = count;
            tied = false;
        } else if (count == best) {
            tied = true;
        }
    }
    return tied ? std::nullopt : selected;
}

std::optional<std::string> label_for(
    const FicIdentityEvidence& evidence, std::uint16_t eid)
{
    const auto labels = evidence.label_counts.find(eid);
    if (labels == evidence.label_counts.end()) return std::nullopt;
    return unique_most_frequent(labels->second);
}

void merge_evidence(FicIdentityEvidence& target,
                    const FicIdentityEvidence& source)
{
    target.available = target.available || source.available;
    target.frames_examined += source.frames_examined;
    target.codewords_decoded += source.codewords_decoded;
    target.valid_fibs += source.valid_fibs;
    target.parsed_figs += source.parsed_figs;
    for (const auto& [value, count] : source.eid_counts)
        target.eid_counts[value] += count;
    for (const auto& [value, count] : source.ecc_counts)
        target.ecc_counts[value] += count;
    for (const auto& [eid, labels] : source.label_counts)
        for (const auto& [label, count] : labels)
            target.label_counts[eid][label] += count;
    for (const auto& [fig, count] : source.fig_counts)
        target.fig_counts[fig] += count;
}

} // namespace

struct FicIdentityAccumulator::Cluster {
    std::size_t id = 0;
    std::string channel;
    std::int64_t timing_sum = 0;
    std::size_t timing_observations = 0;
    int integer_carrier_offset = 0;
    bool spectrum_inverted = false;
    std::size_t contributing_fragments = 0;
    std::size_t last_update = 0;
    bool confirmed = false;
    FicIdentityEvidence evidence;

    std::int64_t timing_center() const
    {
        return timing_observations == 0 ? 0
            : timing_sum / std::int64_t(timing_observations);
    }
};

FicIdentityAccumulator::FicIdentityAccumulator(
    std::int64_t timing_tolerance_samples, std::size_t max_inactive_updates)
    : timing_tolerance_samples_(std::max<std::int64_t>(0, timing_tolerance_samples)),
      max_inactive_updates_(std::max<std::size_t>(1, max_inactive_updates))
{}

FicIdentityAccumulator::~FicIdentityAccumulator() = default;

std::size_t FicIdentityAccumulator::cluster_count() const
{
    return clusters_.size();
}

void FicIdentityAccumulator::prune(
    const std::string& channel, std::size_t channel_update)
{
    clusters_.erase(std::remove_if(clusters_.begin(), clusters_.end(),
        [&](const Cluster& cluster) {
            return cluster.channel == channel &&
                channel_update - cluster.last_update > max_inactive_updates_;
        }), clusters_.end());
}

void FicIdentityAccumulator::begin_capture(const std::string& channel)
{
    const auto channel_update = ++channel_updates_[channel];
    prune(channel, channel_update);
}

std::optional<AccumulatedFicIdentity> FicIdentityAccumulator::add(
    const FicIdentityFragment& fragment)
{
    if (channel_updates_.find(fragment.channel) == channel_updates_.end())
        begin_capture(fragment.channel);
    const auto channel_update = channel_updates_[fragment.channel];
    if (!fragment.evidence.available || fragment.evidence.valid_fibs == 0)
        return std::nullopt;

    const auto incoming_eid = unique_most_frequent(fragment.evidence.eid_counts);
    struct Compatible {
        Cluster* cluster;
        std::int64_t distance;
        std::optional<std::uint16_t> eid;
    };
    std::vector<Compatible> compatible;
    for (auto& cluster : clusters_) {
        if (cluster.channel != fragment.channel ||
            cluster.integer_carrier_offset != fragment.integer_carrier_offset ||
            cluster.spectrum_inverted != fragment.spectrum_inverted)
            continue;
        const auto cluster_eid = unique_most_frequent(cluster.evidence.eid_counts);
        if (incoming_eid && cluster_eid && *incoming_eid != *cluster_eid)
            continue;
        const auto distance = std::llabs(
            fragment.timing_offset_samples - cluster.timing_center());
        if (distance <= timing_tolerance_samples_)
            compatible.push_back({&cluster, distance, cluster_eid});
    }

    // With an incoming EId, prefer its established UEId cluster over an
    // anonymous cluster. Without an EId, never attach an ECC-only fragment
    // when multiple distinct known EIds occupy the same timing tolerance.
    if (!incoming_eid) {
        std::set<std::uint16_t> known_eids;
        for (const auto& value : compatible)
            if (value.eid) known_eids.insert(*value.eid);
        if (known_eids.size() > 1) compatible.clear();
    } else {
        const bool has_exact = std::any_of(
            compatible.begin(), compatible.end(), [&](const auto& value) {
                return value.eid && *value.eid == *incoming_eid;
            });
        if (has_exact) {
            compatible.erase(std::remove_if(
                compatible.begin(), compatible.end(), [&](const auto& value) {
                    return !value.eid || *value.eid != *incoming_eid;
                }), compatible.end());
        }
    }

    Cluster* selected = nullptr;
    if (!compatible.empty()) {
        selected = std::min_element(
            compatible.begin(), compatible.end(), [](const auto& lhs,
                                                       const auto& rhs) {
                return lhs.distance < rhs.distance;
            })->cluster;
    }
    if (!selected) {
        Cluster cluster;
        cluster.id = next_cluster_id_++;
        cluster.channel = fragment.channel;
        cluster.integer_carrier_offset = fragment.integer_carrier_offset;
        cluster.spectrum_inverted = fragment.spectrum_inverted;
        cluster.last_update = channel_update;
        clusters_.push_back(std::move(cluster));
        selected = &clusters_.back();
    }

    selected->timing_sum += fragment.timing_offset_samples;
    ++selected->timing_observations;
    ++selected->contributing_fragments;
    selected->last_update = channel_update;
    merge_evidence(selected->evidence, fragment.evidence);

    const auto eid = unique_most_frequent(selected->evidence.eid_counts);
    const auto ecc = unique_most_frequent(selected->evidence.ecc_counts);
    if (!eid || !ecc || *ecc == 0) return std::nullopt;

    AccumulatedFicIdentity result;
    result.cluster_id = selected->id;
    result.contributing_fragments = selected->contributing_fragments;
    result.total_valid_fibs = selected->evidence.valid_fibs;
    result.timing_center_samples = selected->timing_center();
    result.integer_carrier_offset = selected->integer_carrier_offset;
    result.spectrum_inverted = selected->spectrum_inverted;
    result.newly_confirmed = !selected->confirmed;
    selected->confirmed = true;
    result.ensemble.decoded = true;
    result.ensemble.ueid = (std::uint32_t(*ecc) << 16) | *eid;
    if (const auto label = label_for(selected->evidence, *eid))
        result.ensemble.label = *label;
    result.ensemble.fic = selected->evidence;
    return result;
}

} // namespace dab
