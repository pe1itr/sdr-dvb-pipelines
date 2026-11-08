// The asynchronous front end must preserve samples/gaps and drain at EOF.
#include "dect2/t2/t2rx.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace dect2;
static void check(bool ok, const char* message) {
    if (!ok) { fprintf(stderr, "%s\n", message); std::exit(1); }
}
static void drain(T2Receiver& rx) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (rx.pending() && std::chrono::steady_clock::now() < deadline) {
        rx.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(!rx.pending(), "EOF failed to drain front-end work");
    rx.poll();
}
static RxTelemetry receive(bool pipeline, double rate, bool gaps) {
    T2Receiver rx;
    rx.configure(rate, 8);
    rx.setPipelined(pipeline);
    // More gap markers than queue slots: capacity waits must also consume output.
    for (int i = 0; i < 100; ++i) {
        rx.markGap(17 + i);
        check(rx.pending() <= 32, "front-end queue exceeded its bound");
    }
    drain(rx);
    rx.reset();
    uint32_t rng = 19;
    // Short chunks include cases that cannot produce even one resampled sample.
    // Empty resampler output must not be mistaken for a gap marker.
    for (int i = 0; i < 120; ++i) {
        const size_t count = i % 3 == 0 ? 1 : 4093;
        std::vector<cf32> x(count);
        auto sample = [&] { rng = rng * 1664525u + 1013904223u; return float(int(rng >> 16) - 32768) / 32768; };
        for (auto& v : x) { float re = sample(), im = sample(); v = {re, im}; }
        if (gaps && (i == 53 || i == 79)) rx.markGap(i * 1001);
        rx.feed(x.data(), x.size());
        rx.feed(nullptr, 0);
        check(rx.pending() <= 32, "front-end queue exceeded its bound");
    }
    drain(rx);
    RxTelemetry result;
    rx.telemetry(result, 0);
    check(!result.p1Trace.empty(), "test did not exercise sample processing");
    return result;
}
int main() {
    for (double rate : {10000000.0, 20000000.0, nativeRateHz(8)}) {
        for (bool gaps : {false, true}) {
            auto expected = receive(false, rate, gaps);
            auto actual = receive(true, rate, gaps);
            check(actual.p1Trace == expected.p1Trace, "pipelining changed sample/gap order or lost the EOF tail");
            check(actual.p1Evaluated == expected.p1Evaluated, "P1 processing differs from synchronous front end");
        }
    }
    // Reset while work is queued/in flight, followed by shutdown with pending work.
    for (int repeat = 0; repeat < 20; ++repeat) {
        T2Receiver rx;
        rx.configure(10000000, 8);
        rx.setPipelined(true);
        std::vector<cf32> zero(65536);
        for (int i = 0; i < 3; ++i) rx.feed(zero.data(), zero.size());
        rx.reset();
        rx.feed(zero.data(), zero.size());
        drain(rx);
        rx.setPipelined(false);
        rx.setPipelined(true);
        rx.feed(zero.data(), zero.size());
    }
    puts("pipeline: sample/gap ordering, queue bound, EOF, reset and shutdown passed");
}
