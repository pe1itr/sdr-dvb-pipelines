// Exercise streaming history, phase boundaries and clock changes. The test
// runner compares this checksum with the generic path in a separate process.
#include "dect2/dsp/clock_correct.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace dect2;
int main(int argc, char** argv) {
    std::vector<cf32> input(262144), out;
    uint32_t rng = 7;
    auto sample = [&] { rng = rng * 1664525u + 1013904223u; return float(int(rng >> 16) - 32768) / 32768.f; };
    for (auto& v : input) { float re = sample(), im = sample(); v = {re, im}; }
    ClockCorrector corrector;
    printf("clock backend=%s\n", ClockCorrector::backendName());
    uint64_t hash = 14695981039346656037ull, total = 0;
    auto consume = [&] {
        for (const auto& v : out) {
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) return false;
            const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&v);
            for (size_t j = 0; j < sizeof v; ++j) { hash ^= bytes[j]; hash *= 1099511628211ull; }
        }
        total += out.size(); out.clear(); return true;
    };
    const double ppm[] = {0, -3.7, 3.7, -100, 100, -500, 500};
    const size_t chunks[] = {1, 7, 12, 23, 4093, 65536};
    for (double drift : ppm) {
        corrector.prime(nullptr, 0);
        corrector.setPpm(drift);
        for (size_t pos = 0, block = 0; pos < input.size(); ++block) {
            size_t n = std::min(chunks[block % 6], input.size() - pos);
            corrector.process(input.data() + pos, n, out);
            corrector.process(nullptr, 0, out);
            if (!consume()) return 1;
            pos += n;
            // History resets after a gap, with short and full histories.
            if (block == 9) corrector.prime(input.data() + pos - 3, 3);
            if (block == 14) corrector.prime(input.data() + pos - 24, 24);
            if (block == 16) corrector.setPpm(-drift);
        }
    }
    printf("clock hash=%016llx samples=%llu\n", (unsigned long long)hash, (unsigned long long)total);
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        // Short isolated component measurement, not a live decoder benchmark.
        corrector.prime(nullptr, 0); corrector.setPpm(-3.7);
        auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < 16; ++i) {
            out.clear(); corrector.process(input.data(), input.size(), out);
        }
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        printf("clock benchmark samples=%zu wall_s=%.6f\n", input.size() * 16, seconds);
    }
    return total > 1800000 ? 0 : 1;
}
