// Host adapter for the separately maintained DVB-T2 component (GPL-3.0).
#include "dvbt2_backend.h"
#include "decoder_monitor.h"
#include "dect2/t2/t2rx.h"
#include "dect2/tv/bbunpack.h"
#include "dect2/common/stage_prof.h"
#include "dect2/common/threadpool.h"
#include "dect2/dsp/clock_correct.h"
#include <arpa/inet.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace {
using namespace dect2;
using Clock = std::chrono::steady_clock;
// Long broadcast frames are processed in bursts. Allow more than one maximum
// T2 frame duration, while keeping the live queue strictly bounded (~419 ms).
constexpr size_t chunkSamples = 65536, queueLimit = 64;
struct Chunk { uint64_t first; std::vector<unsigned char> bytes; };
struct Input {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Chunk> queue;
    bool eof = false;
    std::atomic<uint64_t> dropped{0};
};
double number(const char* s) {
    char* end; errno = 0;
    double v = strtod(s, &end);
    return errno || end == s || *end || !std::isfinite(v) ? NAN : v;
}
int monitorSocket() {
    const char* p = getenv("LINRAD_DECODER_MONITOR_PORT");
    double port = p ? number(p) : 0;
    if (!(port >= 1 && port <= 65535 && port == floor(port))) return -1;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((int)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd >= 0 && connect(fd, (sockaddr*)&a, sizeof(a))) { close(fd); return -1; }
    return fd;
}
template<class T> void publish(int fd, const T& msg) {
    if (fd >= 0) (void)send(fd, &msg, sizeof(msg), MSG_DONTWAIT | MSG_NOSIGNAL);
}
}

int dvbt2_backend_main(int argc, char** argv) {
    double rate = 0, bandwidth = 8000000, plp = -1;
    const char* format = "s16";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) {
            fprintf(stderr, "DVB-T2 broadcast: IQ on stdin, MPEG-TS on stdout.\n"
                "--sample-rate HZ --input-format u8|s16|cf32 --bandwidth 8000000 --plp -1..255\n"
                "FEC, constellation and guard interval are detected from L1.\n"
                "Bounded live input: overload drops samples, never slows the SDR clock.\n");
            return 0;
        }
        if (i + 1 == argc) { fprintf(stderr, "DVB-T2: missing option value\n"); return 2; }
        const char* key = argv[i++]; const char* value = argv[i];
        if (!strcmp(key, "--sample-rate")) rate = number(value);
        else if (!strcmp(key, "--bandwidth")) bandwidth = number(value);
        else if (!strcmp(key, "--plp")) plp = number(value);
        else if (!strcmp(key, "--input-format")) format = value;
        else { fprintf(stderr, "DVB-T2: unknown option %s\n", key); return 2; }
    }
    const int sampleBytes = !strcmp(format, "u8") ? 2 : !strcmp(format, "s16") ? 4 :
        (!strcmp(format, "cf32") || !strcmp(format, "f32")) ? 8 : 0;
    if (!sampleBytes || !(rate >= 8000000 && rate <= 20000000) || bandwidth != 8000000 ||
        !(plp >= -1 && plp <= 255 && plp == floor(plp))) {
        fprintf(stderr, "DVB-T2: require 8 MHz bandwidth, 8..20 Msps, u8/s16/cf32 and PLP -1..255\n");
        return 2;
    }
    Input input;
    std::atomic<bool> failed{false};
    const int monitor = monitorSocket();
    std::thread decoder([&] {
        T2Receiver rx; rx.configure(rate, 8); rx.selectPlp((int)plp); rx.setComputeMode(0);
        // Resampling can overlap demodulation, instead of both occupying the
        // one thread that drains the input queue. Keep a synchronous fallback
        // for comparisons with recordings and platform regressions.
        const char* frontend = getenv("LINRAD_DVBT2_FRONTEND");
        const bool pipelined = !frontend || strcmp(frontend, "sync") != 0;
        rx.setPipelined(pipelined);
        fprintf(stderr, "DVB-T2 frontend: %s\n", pipelined ? "pipelined" : "synchronous");
        fprintf(stderr, "DVB-T2 runtime: logical_processors=%u configured_workers=%d profile=%d\n",
            std::thread::hardware_concurrency(), configuredThreads(), prof::stageProfEnabled());
        fprintf(stderr, "DVB-T2 clock correction: %s\n", ClockCorrector::backendName());
        fprintf(stderr, "DVB-T2 LDPC: %s\n", t2LdpcBackendName());
        BbUnpacker unpack;
        uint64_t packets = 0, next = 0, lastPackets = 0, lastGood = 0;
        int previousFrame = -1;
        unpack.setSink([&](const uint8_t* packet) {
            if (failed) return;
            if (fwrite(packet, 1, 188, stdout) != 188) failed = true;
            else ++packets;
        });
        rx.setPlpCallback([&](const PlpResult& r) {
            DECT2_STAGE("host.ts-delivery");
            if (previousFrame >= 0 && r.numT2Frames > 0 &&
                r.t2Frame != (previousFrame + r.frameInterval) % r.numT2Frames) unpack.lost();
            previousFrame = r.t2Frame;
            for (const auto& f : r.frames) {
                if (f.bits.empty() || !f.header.crcOk) unpack.lost();
                else unpack.push(f.bits, f.header);
            }
        });
        RxTelemetry t;
        auto lastStatus = Clock::now(), lastLog = lastStatus, lastProfile = lastStatus;
        float rms = NAN, peak = NAN;
        std::string previousUnsupported;
        std::string previousPlps;
        auto report = [&](bool final) {
            DECT2_STAGE("host.report");
            rx.poll(); // drain completed resampling, then FEC/TS
            rx.telemetry(t, 0); // also collects FEC results and emits TS
            DecoderStatus s{};
            s.magic = DECODER_MONITOR_MAGIC; s.type = DECODER_STATUS;
            s.lock_valid = DECODER_OFDM | DECODER_FEC | DECODER_TS;
            s.locks = (t.state == 2 && t.secSinceP1 < 1 ? DECODER_OFDM : 0) |
                (t.blocksOk > lastGood ? DECODER_FEC : 0) | (packets > lastPackets ? DECODER_TS : 0);
            lastGood = t.blocksOk; lastPackets = packets;
            s.snr_db = t.dataValid ? t.dataSnrDb : t.p2SnrDb;
            s.mer_db = t.plpMerDb; s.pilot_lock = t.p1.metric; s.cfo_hz = t.cfoHz;
            s.rms_dbfs = rms; s.peak_dbfs = peak;
            s.packets = s.written_packets = packets; s.rs_ok = t.blocksOk; s.rs_uncorrectable = t.blocksBad;
            s.input_dropped = input.dropped; s.fec_frames_dropped = t.plpFramesDropped;
            s.plp_id = t.plpSelectedId;
            { std::lock_guard<std::mutex> lock(input.mutex);
              s.buffer_valid = 1; s.buffer_percent = 100.0 * input.queue.size() / queueLimit; }
            static const char* mods[] = {"QPSK", "16QAM", "64QAM", "256QAM"};
            if (t.plpValid) {
                snprintf(s.modulation, sizeof(s.modulation), "%dK %s", t.fftN / 1024,
                    mods[std::clamp(t.plpFec.mod, 0, 3)]);
                snprintf(s.fec, sizeof(s.fec), "%s", rateName(t.plpFec.rate));
            }
            snprintf(s.guard, sizeof(s.guard), "%s", t.giIdx >= 0 ? guardName(t.giIdx) : "auto");
            publish(monitor, s);
            DecoderConstellation c{}; c.magic = DECODER_MONITOR_MAGIC; c.type = DECODER_CONSTELLATION;
            const auto& points = t.plpConst.empty() ? t.eqData : t.plpConst;
            c.count = std::min<size_t>(DECODER_POINTS, points.size());
            for (unsigned i = 0; i < c.count; ++i) { c.iq[2*i] = points[i].real(); c.iq[2*i+1] = points[i].imag(); }
            publish(monitor, c);
            std::string unsupported;
            for (const auto& reason : t.unsupported) unsupported += reason + "; ";
            if (unsupported != previousUnsupported && !unsupported.empty())
                fprintf(stderr, "DVB-T2 unsupported: %s\n", unsupported.c_str());
            previousUnsupported = unsupported;
            std::string plps;
            for (const auto& p : t.plpList) plps += std::to_string(p.id) + (p.supported ? " " : "(unsupported) ");
            if (plps != previousPlps && !plps.empty()) fprintf(stderr, "DVB-T2 available PLPs: %s\n", plps.c_str());
            previousPlps = plps;
            // Live diagnostics must reach the GUI/HTTP log while the worker
            // is running: stopping the live pipeline may terminate it at once.
            if (prof::stageProfEnabled() && (final || Clock::now() - lastProfile >= std::chrono::seconds(5))) {
                auto stages = prof::stageProfSnapshot();
                const auto wallStages = prof::stageProfSnapshot(true);
                std::sort(stages.begin(), stages.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
                for (const auto& stage : stages) {
                    double wall = 0;
                    for (const auto& w : wallStages) if (w.first == stage.first) { wall = w.second.first; break; }
                    fprintf(stderr, "DVB-T2 profile: %s cpu=%.6f calls=%llu wall=%.6f\n", stage.first.c_str(),
                        stage.second.first, (unsigned long long)stage.second.second, wall);
                }
                lastProfile = Clock::now();
            }
            if (final || Clock::now() - lastLog >= std::chrono::seconds(1)) {
                fprintf(stderr, "DVB-T2%s: PLP=%d L1=%d/%d BCH=%llu/%llu TS=%llu input_dropped=%llu fec_frames_dropped=%llu RMS=%.1f SNR=%.1f CFO=%.0f %s %s GI=%s\n",
                    final ? " final" : "", t.plpSelectedId, t.l1preOk, t.l1postOk,
                    (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)packets,
                    (unsigned long long)input.dropped.load(), (unsigned long long)t.plpFramesDropped,
                    rms, s.snr_db, s.cfo_hz, s.modulation, s.fec, s.guard);
                lastLog = Clock::now();
            }
            { DECT2_STAGE("host.ts-flush"); if (fflush(stdout)) failed = true; }
            lastStatus = Clock::now();
        };
        for (;;) {
            Chunk chunk;
            { DECT2_STAGE("host.input-wait");
              std::unique_lock<std::mutex> lock(input.mutex);
              input.ready.wait_for(lock, std::chrono::milliseconds(100), [&] { return input.eof || !input.queue.empty(); });
              if (input.queue.empty()) { if (input.eof) break; }
              else { chunk = std::move(input.queue.front()); input.queue.pop_front(); } }
            if (!chunk.bytes.empty()) {
                if (chunk.first != next) {
                    DECT2_STAGE("host.gap");
                    rx.markGap(chunk.first - next); unpack.lost();
                }
                const size_t n = chunk.bytes.size() / sampleBytes;
                DECT2_STAGE_VAR(convert, "host.convert");
                std::vector<cf32> samples(n);
                double sum = 0, maximum = 0;
                for (size_t i = 0; i < n; ++i) {
                    float iq[2]; const auto* p = chunk.bytes.data() + i * sampleBytes;
                    if (sampleBytes == 2) { iq[0] = (p[0] - 127.5f) / 128; iq[1] = (p[1] - 127.5f) / 128; }
                    else if (sampleBytes == 4) { int16_t v[2]; memcpy(v, p, 4); iq[0] = v[0] / 32768.f; iq[1] = v[1] / 32768.f; }
                    else memcpy(iq, p, 8);
                    if (!std::isfinite(iq[0]) || !std::isfinite(iq[1])) { failed = true; iq[0] = iq[1] = 0; }
                    samples[i] = cf32(iq[0], iq[1]);
                    double power = std::norm(samples[i]); sum += power; maximum = std::max(maximum, power);
                }
                rms = 10 * log10(std::max(sum / n, 1e-20)); peak = 10 * log10(std::max(maximum, 1e-20));
                convert.stop();
                { DECT2_STAGE("host.feed"); rx.feed(samples.data(), samples.size()); }
                next = chunk.first + n;
                { DECT2_STAGE("host.poll"); rx.poll(); }
            }
            if (Clock::now() - lastStatus >= std::chrono::milliseconds(200)) report(false);
        }
        // EOF only: drain resampling and FEC, without inventing more IQ samples.
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (rx.pending() && Clock::now() < deadline) {
            report(false); std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (rx.pending()) { fprintf(stderr, "DVB-T2: receiver drain timed out\n"); failed = true; }
        report(true);
    });
    uint64_t received = 0;
    bool eof = false;
    while (!eof) {
        Chunk chunk{received, std::vector<unsigned char>(chunkSamples * sampleBytes)};
        size_t count = 0;
        while (count < chunk.bytes.size()) {
            ssize_t n = read(STDIN_FILENO, chunk.bytes.data() + count, chunk.bytes.size() - count);
            if (n > 0) count += n;
            else if (n < 0 && errno == EINTR) continue;
            else { if (n < 0) { perror("DVB-T2 input"); failed = true; } eof = true; break; }
        }
        if (count % sampleBytes) { fprintf(stderr, "DVB-T2: truncated IQ sample\n"); failed = true; }
        count -= count % sampleBytes; chunk.bytes.resize(count);
        received += count / sampleBytes;
        if (count) {
            std::lock_guard<std::mutex> lock(input.mutex);
            if (input.queue.size() == queueLimit) input.dropped += count / sampleBytes;
            else input.queue.push_back(std::move(chunk));
        }
        input.ready.notify_one();
    }
    { std::lock_guard<std::mutex> lock(input.mutex); input.eof = true; }
    input.ready.notify_one(); decoder.join();
    if (monitor >= 0) close(monitor);
    return failed ? 1 : 0;
}
