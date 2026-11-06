#pragma once

#include "dab_ensemble.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <map>
#include <string>
#include <vector>

namespace dab {

struct FicIdentityFragment {
    std::string channel;
    std::int64_t timing_offset_samples = 0;
    int integer_carrier_offset = 0;
    bool spectrum_inverted = false;
    FicIdentityEvidence evidence;
};

struct AccumulatedFicIdentity {
    EnsembleInfo ensemble;
    std::size_t cluster_id = 0;
    std::size_t contributing_fragments = 0;
    std::size_t total_valid_fibs = 0;
    std::int64_t timing_center_samples = 0;
    int integer_carrier_offset = 0;
    bool spectrum_inverted = false;
    bool newly_confirmed = false;
};

// In-memory DX protocol accumulator. Before a UEId is known, fragments are
// isolated by channel, refined timing cluster, integer carrier offset and
// spectrum orientation. Once known, a conflicting EId can never enter that
// cluster. Clusters expire after bounded inactivity to avoid joining unrelated
// propagation openings over an unlimited 24/7 run.
class FicIdentityAccumulator {
public:
    explicit FicIdentityAccumulator(std::int64_t timing_tolerance_samples = 504,
                                    std::size_t max_inactive_updates = 64);
    ~FicIdentityAccumulator();

    void begin_capture(const std::string& channel);
    std::optional<AccumulatedFicIdentity> add(
        const FicIdentityFragment& fragment);
    std::size_t cluster_count() const;

private:
    struct Cluster;
    std::int64_t timing_tolerance_samples_;
    std::size_t max_inactive_updates_;
    std::map<std::string, std::size_t> channel_updates_;
    std::size_t next_cluster_id_ = 1;
    std::vector<Cluster> clusters_;

    void prune(const std::string& channel, std::size_t channel_update);
};

} // namespace dab
