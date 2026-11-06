// SPDX-License-Identifier: GPL-3.0-only
// Optional recorded-IQ acquisition regression. Timings are delivered sample
// duration, rounded to 2 ms chunks, not hardware tune or CPU latency.
#include "dx_msc.hpp"
#include "libdxdabsdr.hpp"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>

int main()
{
    const char *directory = std::getenv("DAB_FIXTURE_DIR");
    if (!directory)
    {
        std::cout << "stream-acquisition: SKIP (set DAB_FIXTURE_DIR)\n";
        return 0;
    }
    for (const char *name : {"dab-9c-fic-positive-1s.s16le.iq", "live-tii-phase-11c-4s.s16le.iq",
                             "live-tii-phase-7d-4s.s16le.iq"})
    {
        std::ifstream in(std::string(directory) + "/" + name, std::ios::binary);
        assert(in && "missing requested IQ fixture");
        dab::dx::StreamDemodulator demod;
        dab::dx::MscSubchannel msc;
        dab::dx::SuperframeDecoder sf;
        std::uint64_t samples = 0, first_fic = 0, first_identity = 0, first_au = 0;
        unsigned good_fic = 0, good_au = 0;
        int subchannel = -1;
        std::int16_t raw[8192];
        while (in.read(reinterpret_cast<char *>(raw), sizeof(raw)) || in.gcount())
        {
            std::vector<std::complex<float>> iq(in.gcount() / 4);
            for (std::size_t i = 0; i < iq.size(); ++i)
                iq[i] = {raw[2 * i] / 32768.f, raw[2 * i + 1] / 32768.f};
            samples += iq.size();
            demod.feed(iq.data(), iq.size(),
                       [&](dab::dx::StreamFrame &&f)
                       {
                           if (f.fic_valid)
                           {
                               ++good_fic;
                               if (!first_fic)
                                   first_fic = samples;
                           }
                           if (f.ensemble.decoded && !first_identity)
                               first_identity = samples;
                           if (f.discontinuity)
                           {
                               msc.reset();
                               sf.reset();
                               subchannel = -1;
                           }
                           if (subchannel < 0)
                               for (const auto &service : f.ensemble.metadata.services)
                               {
                                   for (const auto &c : service.components)
                                       if (!c.transport_mode && c.component_type == 63 &&
                                           !c.conditional_access && c.subchannel_id)
                                       {
                                           subchannel = *c.subchannel_id;
                                           break;
                                       }
                                   if (subchannel >= 0)
                                       break;
                               }
                           for (const auto &sub : f.ensemble.metadata.subchannels)
                               if (sub.id == subchannel)
                                   msc.configure(sub);
                           for (unsigned cif = 0; cif < 4; ++cif)
                           {
                               auto word = msc.push(f.msc.data() + 55296 * cif, 55296);
                               if (word.empty())
                                   continue;
                               for (const auto &au : sf.push(std::move(word)))
                                   if (!au.damaged)
                                   {
                                       ++good_au;
                                       if (!first_au)
                                           first_au = samples;
                                   }
                           }
                       });
        }
        assert(good_fic >= 3 && first_identity);
        assert(first_fic <= 430080); // Strong controls: at most 210 ms of IQ.
        if (std::string(name).find("11c") != std::string::npos)
            assert(good_au >= 20 && first_au <= 2048000);
        std::cout << name << ": first FIC=" << first_fic / 2048.0
                  << " ms, ECC/EId=" << first_identity / 2048.0
                  << " ms, CRC-valid audio AU=" << first_au / 2048.0
                  << " ms; good FIC frames=" << good_fic << ", good AUs=" << good_au << '\n';
    }
}
