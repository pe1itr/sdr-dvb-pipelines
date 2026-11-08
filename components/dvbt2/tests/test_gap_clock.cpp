// Regression: resampler resets at known IQ gaps must not steer the sample clock.
// A synthetic 32K signal has exactly zero clock error and no PLP payload.
// Feed it across repeated gaps, then a clean tail; no radio/recording needed.
#include "dect2/t2/t2rx.h"
#include "dect2/t2/t2gen.h"
#include "dect2/dsp/resampler.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;

int main() {
    constexpr size_t block = 65536;
    constexpr double sampleRate = 10000000;
    TxParams p;
    p.s2field1 = 5; // 32K
    p.giIdx = 2;   // 1/8
    p.ext = true;
    p.pp = 6;      // PP7
    p.dataSymbols = 10;
    T2Generator gen(p);
    RationalResampler up;
    up.configure(nativeRateHz(8), sampleRate);
    std::vector<cf32> stream, tmp, out;
    for (int f = 0; f < 40; ++f) {
        gen.nextFrame(tmp);
        out.clear();
        up.process(tmp.data(), tmp.size(), out);
        stream.insert(stream.end(), out.begin(), out.end());
    }

    // Select an absent PLP: this tests synchronization before LDPC.
    T2Receiver rx;
    rx.configure(sampleRate, 8);
    rx.selectPlp(255);
    rx.setPipelined(true);
    RxTelemetry t;
    size_t nextGap = 32 * block;
    double worst = 0;
    for (size_t pos = 0; pos < stream.size();) {
        // Drop one block per nine until 1 s; retain clean input at the end.
        if (pos >= nextGap && pos < size_t(sampleRate)) {
            rx.markGap(block);
            pos += block;
            nextGap = pos + 8 * block;
        }
        size_t n = std::min(block, stream.size() - pos);
        rx.feed(stream.data() + pos, n);
        rx.poll();
        pos += n;
        rx.telemetry(t, 0);
        worst = std::max(worst, std::abs(t.sroPpm));
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (rx.pending() && std::chrono::steady_clock::now() < deadline) {
        rx.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    rx.telemetry(t, 0);
    printf("p1=%llu pre=%llu/%llu post=%llu/%llu SRO=%.3f peak=%.3f SNR=%.3f pending=%d\n",
           (unsigned long long)t.p1Count,
           (unsigned long long)t.l1preGood, (unsigned long long)t.l1preBad,
           (unsigned long long)t.l1postGood, (unsigned long long)t.l1postBad,
           t.sroPpm, worst, t.dataSnrDb, rx.pending());
    return std::abs(t.sroPpm) > 3 || worst > 3 || t.l1preGood < 20 || rx.pending() ? 1 : 0;
}
