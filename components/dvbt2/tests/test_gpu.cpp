// Native GPU correctness gate. Exit 77 means unavailable, never a GPU pass.
#include "dect2/dsp/gpu_ldpc.h"
#include "dect2/t2/t2fec.h"
#include "dect2/common/stable_random.h"
#include <cstdio>
#include <random>
#include <vector>
#include <algorithm>
using namespace dect2;
int main() {
    auto& gpu = GpuLdpc::instance();
    if (!gpu.available()) { puts("GPU LDPC: SKIP (unavailable; CPU fallback)"); return 77; }
    printf("GPU LDPC: %s\n", gpu.deviceName());
    std::mt19937 rng(314159);
    dect2::normal_distribution<float> noise(0, 1);
    unsigned cases=0;
    for (int sh=0; sh<2; ++sh) for (int rate=0; rate<(sh ? 8 : 6); ++rate) {
        PlpFec f; f.shortFrame=sh; f.rate=rate;
        const auto& code=ldpcFor(f);
        // Cross both the old 16-word and new 64-word batch boundaries, then
        // reuse the buffer for a shorter call. Mixed noise tests independent
        // early termination of neighbouring words and stale status handling.
        const std::vector<int> batches = !sh && rate==1 ? std::vector<int>{1,17,65,3} : std::vector<int>{1,17,3};
        for (int nb : batches) for (int pattern=0; pattern<(nb==65 ? 4 : 3); ++pattern) {
            const int limit=pattern==0 ? 30 : 7;
            std::vector<float> llr((size_t)nb*code.n());
            std::vector<uint8_t> sent((size_t)nb*code.n());
            for (int b=0;b<nb;++b) {
                std::vector<uint8_t> word(code.k());
                for (auto& bit:word) bit=rng()&1;
                code.encode(word);
                std::copy(word.begin(),word.end(),sent.begin()+(size_t)b*code.n());
                for (int i=0;i<code.n();++i) {
                    float x=word[i] ? -1.f : 1.f;
                    const float sigma=pattern==3 ? (b%3==0 ? .2f : b%3==1 ? .85f : 1.2f) : pattern==0 ? .2f : .85f;
                    if (pattern!=2) x=2*(x+sigma*noise(rng));
                    if (pattern==2 && i>=200 && i<560) x=(i&1) ? -0.f : 0.f;
                    llr[(size_t)b*code.n()+i]=x;
                }
            }
            std::vector<uint8_t> hard(llr.size()), ok(nb);
            std::vector<int> iterations(nb);
            if (!gpu.decode(code,llr.data(),nb,limit,hard.data(),ok.data(),iterations.data())) {
                fprintf(stderr,"GPU call failed: short=%d rate=%d batch=%d\n",sh,rate,nb); return 1;
            }
            for (int b=0;b<nb;++b) {
                auto start=llr.begin()+(size_t)b*code.n();
                std::vector<float> input(start,start+code.n());
                std::vector<uint8_t> reference; int it=0;
                const bool good=t2LdpcDecode(code,input,limit,reference,&it);
                if (good!=(bool)ok[b] || it!=iterations[b] ||
                    !std::equal(reference.begin(),reference.end(),hard.begin()+(size_t)b*code.n()) ||
                    (pattern==0 && (!ok[b] || !std::equal(reference.begin(),reference.end(),sent.begin()+(size_t)b*code.n())))) {
                    fprintf(stderr,"GPU/CPU mismatch: short=%d rate=%d batch=%d pattern=%d block=%d ok=%d/%d iterations=%d/%d\n",
                        sh,rate,nb,pattern,b,good,ok[b],it,iterations[b]); return 1;
                }
                ++cases;
            }
        }
        printf("GPU LDPC: short=%d rate=%s passed\n",sh,rateName(rate)); fflush(stdout);
    }
    printf("GPU LDPC: %u CPU-equivalent cases passed\n",cases);
    return 0;
}
