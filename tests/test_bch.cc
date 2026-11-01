// Exercise every DVB-S2 BCH length, including maximum correctable errors
// in data and parity. The encoder still uses bitwise polynomial division.
#include <unistd.h>
#include "leansdr/framework.h"
#include "leansdr/generic.h"
#include "leansdr/dsp.h"
#include "leansdr/sdr.h"
#include "leansdr/dvb.h"
#include "leansdr/dvbs2.h"
#include <cassert>
#include <random>
#include <set>

int main() {
    using namespace leansdr;
    s2_bch_engines engines;
    std::mt19937 random(41);
    unsigned cases = 0;
    for (int sf=0; sf<2; ++sf) {
        for (int fec=0; fec<FEC_COUNT; ++fec) {
            auto engine = engines.bchs[sf][fec];
            if (!engine) continue;
            const auto &info = fec_infos[sf][fec];
            std::vector<uint8_t> code(info.kldpc/8);
            for (int pattern=0; pattern<3; ++pattern) {
                for (int i=0; i<info.Kbch/8; ++i)
                    code[i] = pattern==0 ? 0 : pattern==1 ? 255 : random();
                engine->encode(code.data(), info.Kbch/8, code.data()+info.Kbch/8);
                for (int errors : {0, 1, info.t/2, info.t, info.t+1}) {
                    auto damaged = code;
                    std::set<unsigned> positions;
                    if (errors) positions.insert(0);
                    if (errors>1) positions.insert(info.kldpc-1);
                    while (positions.size()<unsigned(errors))
                        positions.insert(random()%info.kldpc);
                    for (unsigned bit : positions)
                        damaged[bit/8] ^= 128 >> (bit%8);
                    int corrected = engine->decode(damaged.data(), damaged.size());
                    if (errors <= info.t) {
                        assert(corrected == errors);
                        assert(damaged == code);
                    } else {
                        // These deterministic beyond-limit patterns must fail.
                        assert(corrected == -1);
                    }
                    ++cases;
                }
            }
        }
    }
    printf("BCH: %u clean/correctable/uncorrectable cases passed across all 21 DVB-S2 tables\n", cases);
}
