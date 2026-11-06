// SPDX-License-Identifier: GPL-3.0-only
#include "dab_plus_radio.hpp"
#include <cassert>
#include <iostream>
#include <neaacdec.h>
int main()
{
    // FAAD2 parses all legal mono/stereo/SBR/PS configurations with 960 core samples.
    for (unsigned h : {0x00, 0x10, 0x40, 0x50, 0x20, 0x30, 0x60, 0x70, 0x28, 0x68})
    {
        auto asc = dab::dab_plus_audio_specific_config(h);
        mp4AudioSpecificConfig config{};
        assert(NeAACDecAudioSpecificConfig(asc.data(), asc.size(), &config) == 0);
        assert(config.frameLengthFlag == 1);
        auto decoder = NeAACDecOpen();
        unsigned long rate = 0;
        unsigned char channels = 0;
        assert(static_cast<signed char>(
                   NeAACDecInit2(decoder, asc.data(), asc.size(), &rate, &channels)) >= 0);
        NeAACDecClose(decoder);
    }
    dab::DabPlusRadioSession radio("0x0", "null", false);
    assert(!radio.fic_synced() && !radio.station_selected() && radio.services().empty());
    std::vector<std::complex<float>> iq(32768, {0.1f, 0.2f});
    for (unsigned i = 0; i < 126; ++i)
        radio.push_samples(iq.data(), iq.size());
    auto metrics = radio.metrics();
    assert(metrics.iq_samples == 4096000 && metrics.dropped_iq == 1);
    radio.select_station("0x1234");
    assert(!radio.station_selected());
    radio.start(227360);
    radio.finish();
    radio.stop();
    radio.check_error();
    assert(radio.decoded_audio_frames() == 0);
    std::cout << "dab-plus-radio: FAAD2 configuration, bounded input, drain and lifecycle PASS\n";
}
