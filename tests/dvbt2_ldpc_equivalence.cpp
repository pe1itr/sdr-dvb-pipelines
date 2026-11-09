// Guard the optimized T2 LDPC against the independent full-layer decoder.
// Compare convergence, iteration count and every output bit, including failures.
#include "dect2/t2/t2fec.h"
#include <cstdio>
#include <cmath>
#include <random>
#include <vector>
using namespace dect2;
int main() {
    std::mt19937 rng(271828);
    std::normal_distribution<float> noise(0, 1);
    unsigned cases=0;
    for (int sh=0; sh<2; ++sh) for (int rate=0; rate<(sh ? 8 : 6); ++rate) {
        PlpFec f; f.shortFrame=sh; f.rate=rate;
        const auto& code=ldpcFor(f);
        int blocked=0;
        for (const auto& layer:code.layers()) {
            bool unique=layer.size()+2<=64;
            for (size_t a=0;a<layer.size();++a) for (size_t b=a+1;b<layer.size();++b)
                if (layer[a].group==layer[b].group) unique=false;
            blocked+=unique;
        }
        printf("short=%d rate=%s: %d/%d independent layers\n",sh,rateName(rate),blocked,code.q());
        std::vector<uint8_t> sent(code.k());
        for (auto& bit:sent) bit=rng() & 1;
        code.encode(sent);
        for (float sigma : {.25f, .85f, 1.4f}) {
            std::vector<float> llr(code.n());
            for (size_t i=0; i<llr.size(); ++i) llr[i]=2*((sent[i] ? -1.f : 1.f)+sigma*noise(rng))/(sigma*sigma);
            // An erased burst plus exact ties also exercise the minimum selection.
            for (int i=200; i<560; ++i) llr[i]=0;
            for (float alpha : {.66f,.78f,.9f}) for (int limit : {1,2,50,150}) {
                std::vector<uint8_t> reference, actual; int ri=0, ai=0;
                bool rok=code.decodeFast(llr,limit,reference,&ri,alpha);
                bool aok=t2LdpcDecode(code,llr,limit,actual,&ai,alpha);
                ++cases;
                if (rok!=aok || ri!=ai || reference!=actual) {
                    fprintf(stderr,"Mismatch: short=%d rate=%s noise=%g alpha=%g limit=%d ref=%d/%d actual=%d/%d\n",
                        sh,rateName(rate),sigma,alpha,limit,rok,ri,aok,ai); return 1;
                }
            }
        }
        // Value-based minimum selection must also preserve ties, signed zero,
        // tiny inputs and values at the minimum accumulator's initial bound.
        for (int pattern=0; pattern<4; ++pattern) {
            std::vector<float> llr(code.n());
            const float magnitude[] = {0.f, 1.f, 1e-30f, 1e30f};
            for (int i=0; i<code.n(); ++i)
                llr[i]=std::copysign(magnitude[pattern], sent[i] ? -1.f : 1.f);
            for (int i=200; i<560; ++i) llr[i]=(i&1) ? -0.f : 0.f;
            for (float alpha : {.66f,.78f,.9f}) for (int limit : {1,2,7}) {
                std::vector<uint8_t> reference, actual; int ri=0, ai=0;
                bool rok=code.decodeFast(llr,limit,reference,&ri,alpha);
                bool aok=t2LdpcDecode(code,llr,limit,actual,&ai,alpha);
                ++cases;
                if (rok!=aok || ri!=ai || reference!=actual) {
                    fprintf(stderr,"Tie mismatch: short=%d rate=%s pattern=%d alpha=%g limit=%d\n",
                        sh,rateName(rate),pattern,alpha,limit); return 1;
                }
            }
        }
    }
    printf("DVB-T2 LDPC: %u cases identical (14 normal/short rates, noise, erasures, alpha, iteration limits)\n",cases);
}
