// The asynchronous front end must preserve samples/gaps and drain at EOF.
#include "dect2/t2/t2rx.h"
#include "dect2/t2/plp_cells.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
using namespace dect2;
static void check(bool ok, const char* message) {
    if (!ok) { fprintf(stderr, "%s\n", message); std::exit(1); }
}
static void plpSelection() {
    unsigned cases = 0;
    for (size_t np : {0u, 1u, 5u, 32u}) for (size_t nd : {0u, 1u, 7u, 65u}) {
        std::vector<cf32> p2(np), power(np), data(nd);
        std::vector<float> dn(nd);
        for (size_t i=0; i<np; ++i) { p2[i] = cf32(float(i), -float(i)); power[i] = float(int(i%5)-1); }
        for (size_t i=0; i<nd; ++i) { data[i] = cf32(float(i+100), -float(i)); dn[i] = float(i)*0.125f; }
        for (double sigma : {0.0, 0.001, 5.0}) {
            // Original concatenate-then-slice path is the independent oracle.
            auto joined = p2;
            joined.insert(joined.end(), data.begin(), data.end());
            std::vector<float> noise(np);
            for (size_t i=0; i<np; ++i) noise[i] = (float)(sigma/(2.0*std::max(1e-12f,power[i].real())));
            noise.insert(noise.end(), dn.begin(), dn.end());
            std::vector<cf32> out;
            std::vector<float> n0;
            for (size_t start=0; start<=joined.size(); ++start) for (size_t count=0; count<=joined.size()-start; ++count) {
                check(selectPlpCells(p2,power,data,dn,start,count,sigma,out,n0), "valid PLP range rejected");
                check(out.size()==count && n0.size()==count, "wrong PLP selection size");
                if (count) {
                    check(!std::memcmp(out.data(),joined.data()+start,count*sizeof(cf32)), "PLP cells changed");
                    check(!std::memcmp(n0.data(),noise.data()+start,count*sizeof(float)), "PLP noise changed");
                }
                ++cases;
            }
            check(!selectPlpCells(p2,power,data,dn,joined.size()+1,0,sigma,out,n0), "invalid PLP start accepted");
            check(!selectPlpCells(p2,power,data,dn,0,std::numeric_limits<size_t>::max(),sigma,out,n0), "overflowing PLP count accepted");
        }
    }
    std::vector<cf32> p2{{1,2}}, power{{1,0}}, data{{3,4}}, out;
    std::vector<float> dn{0.5f}, n0;
    check(selectPlpCells(p2,power,data,dn,0,2,1.0,out,n0), "cross-boundary PLP failed");
    p2[0]=data[0]=cf32(99,99); dn[0]=99;
    check(out[0]==cf32(1,2) && out[1]==cf32(3,4) && n0[1]==0.5f, "PLP job borrowed reusable input");
    power.clear();
    check(!selectPlpCells(p2,power,data,dn,0,1,1.0,out,n0), "incomplete P2 powers accepted");
    printf("PLP selection: %u ranges byte-identical; ownership and bounds passed\n",cases);
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
    plpSelection();
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
