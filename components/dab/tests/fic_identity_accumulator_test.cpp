#include "fic_identity_accumulator.hpp"

#include <cassert>
#include <iostream>

namespace {

dab::FicIdentityEvidence evidence()
{
    dab::FicIdentityEvidence value;
    value.available = true;
    value.valid_fibs = 1;
    value.parsed_figs = 1;
    return value;
}

dab::FicIdentityFragment fragment(
    std::int64_t timing, dab::FicIdentityEvidence value,
    int carrier = 0, bool inverted = false)
{
    return {"11D", timing, carrier, inverted, std::move(value)};
}

} // namespace

int main()
{
    // EId and ECC may arrive in different CRC-valid captures. A label is not
    // required to establish the UEId.
    dab::FicIdentityAccumulator accumulator(100, 16);
    auto eid = evidence();
    eid.eid_counts[0xC181] = 1;
    accumulator.begin_capture("11D");
    assert(!accumulator.add(fragment(1400, eid)));

    auto ecc = evidence();
    ecc.ecc_counts[0xE1] = 1;
    accumulator.begin_capture("11D");
    const auto identified = accumulator.add(fragment(1450, ecc));
    assert(identified);
    assert(identified->newly_confirmed);
    assert(identified->ensemble.decoded);
    assert(identified->ensemble.ueid == 0xE1C181);
    assert(identified->ensemble.label.empty());
    assert(identified->contributing_fragments == 2);
    assert(identified->total_valid_fibs == 2);

    // FIG 1/0 carries its own EId and may enrich the already-confirmed
    // identity later, even when FIG 0/0 was absent from that fragment.
    auto label = evidence();
    label.eid_counts[0xC181] = 1;
    label.label_counts[0xC181]["D1 National"] = 1;
    accumulator.begin_capture("11D");
    const auto enriched = accumulator.add(fragment(1420, label));
    assert(enriched);
    assert(!enriched->newly_confirmed);
    assert(enriched->ensemble.label == "D1 National");

    // A label-only first fragment must establish the provisional EId cluster
    // so a later CRC-valid FIG 0/9 can complete the UEId.
    dab::FicIdentityAccumulator label_first(100, 16);
    auto early_label = evidence();
    early_label.eid_counts[0x6ADA] = 1;
    early_label.label_counts[0x6ADA]["DigiRadioAntwerp"] = 1;
    label_first.begin_capture("10A");
    assert(!label_first.add(fragment(17601, early_label)));
    auto later_ecc = evidence();
    later_ecc.ecc_counts[0xE2] = 1;
    label_first.begin_capture("10A");
    const auto label_identified =
        label_first.add(fragment(17620, later_ecc));
    assert(label_identified);
    assert(label_identified->ensemble.ueid == 0xE26ADA);
    assert(label_identified->ensemble.label == "DigiRadioAntwerp");

    // A conflicting EId at the same RF timing must not contaminate the first
    // identity cluster.
    auto other_eid = evidence();
    other_eid.eid_counts[0xD202] = 1;
    accumulator.begin_capture("11D");
    assert(!accumulator.add(fragment(1410, other_eid)));
    auto other_ecc = evidence();
    other_ecc.eid_counts[0xD202] = 1;
    other_ecc.ecc_counts[0xE0] = 1;
    accumulator.begin_capture("11D");
    const auto other = accumulator.add(fragment(1415, other_ecc));
    assert(other);
    assert(other->ensemble.ueid == 0xE0D202);
    assert(other->cluster_id != identified->cluster_id);

    auto ambiguous_ecc = evidence();
    ambiguous_ecc.ecc_counts[0xE4] = 1;
    accumulator.begin_capture("11D");
    assert(!accumulator.add(fragment(1412, ambiguous_ecc)));

    // Carrier-bin and spectrum hypotheses are part of the provisional key.
    auto isolated = evidence();
    isolated.eid_counts[0xAAAA] = 1;
    accumulator.begin_capture("11D");
    assert(!accumulator.add(fragment(1400, isolated, 1, false)));
    auto isolated_ecc = evidence();
    isolated_ecc.ecc_counts[0xE2] = 1;
    accumulator.begin_capture("11D");
    assert(!accumulator.add(fragment(1400, isolated_ecc, 0, true)));

    // Ambiguous tied values cannot produce an identity.
    dab::FicIdentityAccumulator ambiguous;
    auto tied = evidence();
    tied.eid_counts[0x1111] = 1;
    tied.eid_counts[0x2222] = 1;
    tied.ecc_counts[0xE0] = 1;
    ambiguous.begin_capture("11D");
    assert(!ambiguous.add(fragment(0, tied)));

    std::cout << "fic-identity-accumulator-test: PASS\n";
}
