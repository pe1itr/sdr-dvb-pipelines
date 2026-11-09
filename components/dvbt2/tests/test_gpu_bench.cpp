// Offline component benchmark, with live receivers stopped. Includes transfers;
// CPU uses the receiver's background worker pool. Not a live reception claim.
#include "dect2/dsp/gpu_ldpc.h"
#include "dect2/t2/t2fec.h"
#include "dect2/common/threadpool.h"
#include "dect2/common/stable_random.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>
using namespace dect2;
int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr,"usage: test_gpu_bench cpu|gpu noise_sigma\n"); return 2; }
    const bool gpuMode = !strcmp(argv[1],"gpu");
    if (!gpuMode && strcmp(argv[1],"cpu")) return 2;
    const float sigma = atof(argv[2]);
    if (!(sigma > 0 && sigma < 3)) return 2;
    PlpFec f; f.shortFrame=false; f.rate=1;
    const auto& code=ldpcFor(f);
    const int nb=183, n=code.n();
    std::vector<float> input((size_t)nb*n);
    std::vector<uint8_t> sent(input.size()), hard(input.size()), ok(nb);
    std::vector<int> iterations(nb);
    std::mt19937 rng(654321);
    dect2::normal_distribution<float> noise(0,1);
    for (int b=0;b<nb;++b) {
        std::vector<uint8_t> word(code.k());
        for (auto& bit:word) bit=rng()&1;
        code.encode(word);
        std::copy(word.begin(),word.end(),sent.begin()+(size_t)b*n);
        for (int i=0;i<n;++i) input[(size_t)b*n+i]=2*((word[i] ? -1.f : 1.f)+sigma*noise(rng))/(sigma*sigma);
    }
    GpuLdpc* gpu=gpuMode ? &GpuLdpc::instance() : nullptr;
    if (gpu && !gpu->available()) return 77;
    std::vector<double> times;
    for (int run=0;run<4;++run) {
        auto start=std::chrono::steady_clock::now();
        if (gpu) {
            if (!gpu->decode(code,input.data(),nb,50,hard.data(),ok.data(),iterations.data())) return 1;
        } else {
            ThreadPool::background().parallelFor(nb,[&](size_t b) {
                std::vector<float> llr(input.begin()+b*n,input.begin()+(b+1)*n);
                std::vector<uint8_t> bits;
                ok[b]=t2LdpcDecode(code,llr,50,bits,&iterations[b]);
                std::copy(bits.begin(),bits.end(),hard.begin()+b*n);
            });
        }
        double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if (run) times.push_back(ms); // exclude table/buffer initialization
    }
    std::sort(times.begin(),times.end());
    size_t errors=0, undetected=0;
    for (int b=0;b<nb;++b) for (int i=0;i<n;++i) if (hard[(size_t)b*n+i]!=sent[(size_t)b*n+i]) {
        ++errors; if (ok[b]) ++undetected;
    }
    printf("{\"mode\":\"%s\",\"sigma\":%.3f,\"codewords\":%d,\"median_ms\":%.3f,\"coded_Mbps\":%.3f,\"converged\":%d,\"mean_iterations\":%.3f,\"bit_errors\":%zu,\"undetected_errors\":%zu}\n",
        argv[1],sigma,nb,times[1],nb*(double)n/times[1]/1000,
        std::accumulate(ok.begin(),ok.end(),0),std::accumulate(iterations.begin(),iterations.end(),0)/(double)nb,errors,undetected);
    return undetected ? 1 : 0;
}
