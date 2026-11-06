#include "dab_plus_radio.hpp"

#include <dabsdr.h>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

int no_service(dabsdrHandle_t, std::uint8_t, dabsdrServiceListItem_t*)
{
    return EXIT_FAILURE;
}

} // namespace

int main()
{
    // AAC-LC, 32 kHz, mono, 960-sample transform.
    assert(dab::dab_plus_audio_specific_config(0x00) ==
           std::vector<std::uint8_t>({0x12, 0x8c}));

    // HE-AAC, 48 kHz, stereo: 24 kHz AAC core plus explicit SBR.
    assert(dab::dab_plus_audio_specific_config(0x70) ==
           std::vector<std::uint8_t>({0x13, 0x14, 0x56, 0xe5, 0x98}));

    // HE-AAC v2, 48 kHz: mono core plus explicit SBR and PS signalling.
    assert(dab::dab_plus_audio_specific_config(0x68) ==
           std::vector<std::uint8_t>(
               {0x13, 0x0c, 0x56, 0xe5, 0x9d, 0x48, 0x80}));

    // Radio diagnostics distinguish RF/null synchronization from full FIC
    // synchronization, and an empty early list is not considered discovery.
    dab::DabPlusRadioSession radio("0xffffffff", "null", false);
    // Exercise queue pressure without starting decoder/audio consumer threads.
    auto metrics=radio.metrics();
    assert(metrics.iq_samples==0 && metrics.iq_peak_samples==0);
    assert(metrics.audio_frames==0 && metrics.alsa_underruns==0);
    std::vector<std::complex<float>> block(32768, {0.25f, -0.25f});
    const auto blocks=metrics.iq_capacity_samples/block.size();
    for (std::size_t i=0;i<blocks+1;++i) radio.push_samples(block.data(),block.size());
    metrics=radio.metrics();
    assert(metrics.iq_samples==metrics.iq_capacity_samples);
    assert(metrics.iq_peak_samples==metrics.iq_capacity_samples);
    assert(metrics.dropped_iq==1);
    float output[512];
    radio.supply_input(output,256);
    metrics=radio.metrics();
    assert(metrics.iq_samples==metrics.iq_capacity_samples-256);
    assert(metrics.iq_peak_samples==metrics.iq_capacity_samples);
    assert(output[0]==8192.0f && output[1]==-8192.0f);
    dabsdrAudioCBData_t audio{};
    audio.id=DABSDR_ID_AUDIO_PRIMARY;audio.ASCTy=63;
    for (std::size_t i=0;i<metrics.audio_capacity_frames+3;++i) radio.handle_audio(&audio);
    metrics=radio.metrics();
    assert(metrics.audio_frames==metrics.audio_capacity_frames);
    assert(metrics.audio_peak_frames==metrics.audio_capacity_frames);
    assert(metrics.dropped_audio==3 && metrics.alsa_underruns==0);
    assert(!radio.dab_signal_synced());
    assert(!radio.fic_synced());
    dabsdrNtfSyncStatus_t sync{};
    sync.syncLevel = DABSDR_SYNC_LEVEL_ON_NULL;
    dabsdrNotificationCBData_t notification{
        DABSDR_NID_SYNC_STATUS, DABSDR_NSTAT_SUCCESS,
        std::uint16_t(sizeof(sync)), &sync};
    radio.handle_notification(&notification);
    assert(radio.dab_signal_synced());
    assert(!radio.fic_synced());
    sync.syncLevel = DABSDR_SYNC_LEVEL_FIC;
    radio.handle_notification(&notification);
    assert(radio.fic_synced());

    dabsdrNtfServiceList_t empty_list{0, no_service};
    notification = {DABSDR_NID_SERVICE_LIST, DABSDR_NSTAT_SUCCESS,
                    std::uint16_t(sizeof(empty_list)), &empty_list};
    radio.handle_notification(&notification);
    assert(!radio.service_list_received());
    assert(!radio.discovery_complete());

    std::cout << "dab-plus-radio-test: PASS\n";
}
