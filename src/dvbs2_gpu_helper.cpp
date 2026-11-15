// Optional PI6EHV GPU helper. CPU remains the default receiver backend.
// FP32 normalized min-sum differs from production S2 int8 corrected min-sum.
#include "ldpc_pool.h"
#include "dect2/dsp/gpu_ldpc.h"
#include "dect2/t2/t2fec.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
// EOF is valid only between complete framed requests.
int readFull(void* buffer, size_t bytes, bool allowEof) {
    auto* p = static_cast<unsigned char*>(buffer);
    size_t used = 0;
    while (used < bytes) {
        const ssize_t n = read(STDIN_FILENO, p + used, bytes - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { perror("S2 GPU helper read"); return -1; }
        if (!n) {
            if (!used && allowEof) return 0;
            fprintf(stderr, "S2 GPU helper: truncated request\n"); return -1;
        }
        used += static_cast<size_t>(n);
    }
    return 1;
}
bool writeFull(const void* buffer, size_t bytes) {
    auto* p = static_cast<const unsigned char*>(buffer);
    while (bytes) {
        const ssize_t n = write(STDOUT_FILENO, p, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { perror("S2 GPU helper write"); return false; }
        p += n; bytes -= static_cast<size_t>(n);
    }
    return true;
}
int run(int argc, char** argv) {
    bool framed = false, cpu = false, shortFrame = false;
    int modcod = -1;
#ifdef S2_GPU_STRICT
    bool strict = true;
#else
    bool strict = false;
#endif
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) {
            fprintf(stdout,
                "Experimental S2 LDPC helper: --framed --modcod N [--backend gpu|cpu-reference] [--require-gpu]\n"
                "Native uint32 batch size 1..32, then int8 LLRs; --shortframes selects 16200 bits.\n"
                "Returns the same number of int8 decision frames, no output header.\n"
                "FP32 normalized min-sum, alpha 0.78, 25 iterations; NOT production S2 arithmetic.\n"
                "cpu-reference is the matching FP32 algorithm, NOT the production CPU baseline.\n"
                "GPU supports MODCOD 14 normal frames. Other modes use the production CPU helper.\n"
                "Nonconverged frames retry on CPU. --require-gpu disables fallback for experiments.\n");
            return 0;
        }
        if (!strcmp(argv[i], "--framed")) framed = true;
        else if (!strcmp(argv[i], "--require-gpu")) strict = true;
        else if (!strcmp(argv[i], "--shortframes")) shortFrame = true;
        else if (!strcmp(argv[i], "--modcod") && i + 1 < argc) {
            char* end = nullptr;
            long value = strtol(argv[++i], &end, 10);
            if (!*argv[i] || *end || value < 1 || value > 28) return 2;
            modcod = static_cast<int>(value);
        } else if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            const char* value = argv[++i];
            if (strcmp(value, "gpu") && strcmp(value, "cpu-reference")) return 2;
            cpu = !strcmp(value, "cpu-reference");
        } else {
            fprintf(stderr, "S2 GPU helper: unsupported option/value: %s\n", argv[i]);
            return 2;
        }
    }
    if (!framed || modcod < 0 || ((strict || cpu) && (modcod != 14 || shortFrame))) {
        fprintf(stderr, "S2 GPU helper: requires --framed --modcod 14; normal frames only\n");
        return 2;
    }
    dect2::PlpFec fec; fec.rate = 3; fec.shortFrame = false;
    const auto& code = dect2::ldpcFor(fec);
    if (code.n() != 64800 || code.k() != 48600) return 1;
    const bool supported = modcod == 14 && !shortFrame;
    const size_t frameBytes = shortFrame ? 16200 : 64800;
    std::unique_ptr<LdpcPool, decltype(&ldpc_pool_destroy)> fallback(nullptr, ldpc_pool_destroy);
    uint64_t cpuFrames = 0, gpuFrames = 0;
    auto retryCpu = [&](std::vector<int8_t>& data, const std::vector<unsigned>& indices) {
        if (indices.empty()) return;
        if (!fallback) {
            char path[4096];
            const ssize_t n = readlink("/proc/self/exe", path, sizeof(path)-1);
            if (n <= 0) throw std::runtime_error("cannot locate CPU helper");
            path[n] = 0;
            std::string command(path);
            command = command.substr(0, command.find_last_of('/')) + "/ldpc_tool";
            if (access(command.c_str(), X_OK)) throw std::runtime_error("missing production CPU helper");
            fallback.reset(ldpc_pool_create(command.c_str(), 1, 32, 0));
            fprintf(stderr, "DVB-S2 LDPC: CPU fallback/retry active, original LLR input retained\n");
        }
        std::vector<std::unique_ptr<LdpcFrame>> jobs;
        for (unsigned b : indices) {
            auto f = std::make_unique<LdpcFrame>();
            f->modcod = modcod; f->short_frame = shortFrame;
            f->data.assign(data.begin()+b*frameBytes, data.begin()+(b+1)*frameBytes);
            jobs.push_back(std::move(f));
        }
        std::vector<LdpcFrame *> ready;
        for (auto& f : jobs) ready.push_back(f.get());
        ldpc_pool_submit_batch(fallback.get(), ready);
        // All jobs must finish before their storage is released, even on failure.
        bool failed = false;
        for (size_t i = 0; i < jobs.size(); ++i) {
            auto& f = jobs[i];
            while (!f->done.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            failed |= f->failed;
            std::copy(f->data.begin(), f->data.end(), data.begin()+indices[i]*frameBytes);
        }
        if (failed) throw std::runtime_error("production CPU helper failed");
        cpuFrames += indices.size();
    };
    dect2::GpuLdpc* gpu = nullptr;
    uint64_t batches = 0, frames = 0, converged = 0, iterations = 0;
    double milliseconds = 0;
    const bool profile = getenv("LINRAD_S2_LDPC_PROFILE") != nullptr;
    fprintf(stderr, "S2 experimental LDPC: %s, FP32 normalized min-sum, alpha=0.78 iterations=25\n",
            cpu ? "CPU reference (not production CPU)" : strict ? "GPU required" : "GPU requested, CPU fallback enabled");
    if (!supported) fprintf(stderr, "DVB-S2 LDPC: unsupported GPU MODCOD/frame size; using CPU\n");
    bool gpuDisabled = !supported;
    for (;;) {
        uint32_t count;
        const int status = readFull(&count, sizeof(count), true);
        if (status < 0) return 1;
        if (!status) break;
        if (!count || count > 32) {
            fprintf(stderr, "S2 GPU helper: batch length must be 1..32\n"); return 2;
        }
        std::vector<int8_t> input(static_cast<size_t>(count) * frameBytes);
        if (readFull(input.data(), input.size(), false) != 1) return 1;
        // Initialize only after a valid request, outside measured decode latency.
        if (!cpu && !gpuDisabled && !gpu) {
            const auto initStart = std::chrono::steady_clock::now();
            gpu = &dect2::GpuLdpc::instance();
            if (!gpu->available()) {
                fprintf(stderr, "S2 GPU helper: GPU unavailable%s\n", strict ? "; experiment aborted" : "; using CPU");
                if (strict) return 77;
                gpuDisabled = true;
            }
            if (!gpuDisabled) fprintf(stderr, "S2 GPU helper: device=%s\n", gpu->deviceName());
            fprintf(stderr, "S2 GPU initialization: %.3f ms\n",
                    std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-initStart).count());
        }
        const auto start = std::chrono::steady_clock::now();
        std::vector<unsigned> retries;
        if (gpuDisabled) {
            for (uint32_t b = 0; b < count; ++b) retries.push_back(b);
            retryCpu(input, retries);
        } else {
            std::vector<float> llr(input.begin(), input.end());
            std::vector<uint8_t> hard(input.size()), ok(count);
            std::vector<int> used(count);
            if (cpu) {
                for (uint32_t b = 0; b < count; ++b) {
                    const size_t offset = static_cast<size_t>(b) * code.n();
                    std::vector<float> word(llr.begin() + offset, llr.begin() + offset + code.n());
                    std::vector<uint8_t> bits;
                    ok[b] = code.decodeFast(word, 25, bits, &used[b], 0.78f, false);
                    std::copy(bits.begin(), bits.end(), hard.begin() + offset);
                }
            } else if (!gpu->decode(code, llr.data(), count, 25, hard.data(), ok.data(), used.data(), 0.78f, false)) {
                fprintf(stderr, "S2 GPU helper: device call failed%s\n", strict ? "; experiment aborted" : "; using CPU");
                if (strict) return 1;
                gpuDisabled = true;
            }
            // Only the signs are consumed by S2 BCH. Emit nonzero decision bytes;
            // retain failed-parity frames because BCH may still recover them.
            for (uint32_t b = 0; b < count; ++b) {
                if (!strict && !cpu && (gpuDisabled || !ok[b])) {
                    retries.push_back(b); // Preserve original soft values for CPU/BCH.
                } else {
                    for (size_t i = b*frameBytes; i < (b+1)*frameBytes; ++i)
                        input[i] = hard[i] ? -127 : 127;
                    if (!cpu) ++gpuFrames;
                }
            }
            retryCpu(input, retries);
            for (uint32_t b = 0; b < count; ++b) { converged += ok[b] != 0; iterations += used[b]; }
        }
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
        milliseconds += elapsed;
        if (profile) fprintf(stderr,"[s2-ldpc] frames=%u host_ms=%.6f cpu_retries=%zu\n",count,elapsed,retries.size());
        ++batches; frames += count;
        if (batches == 1 || batches % 256 == 0)
            fprintf(stderr, "DVB-S2 LDPC backend: gpu_frames=%llu cpu_frames=%llu\n",
                    (unsigned long long)gpuFrames, (unsigned long long)cpuFrames);
        if (!writeFull(input.data(), input.size())) return 1;
    }
    fprintf(stderr, "S2 helper totals: batches=%llu frames=%llu converged=%llu iterations=%llu host_decode_ms=%.3f\n",
            (unsigned long long)batches, (unsigned long long)frames,
            (unsigned long long)converged, (unsigned long long)iterations, milliseconds);
    fprintf(stderr, "DVB-S2 LDPC backend: gpu_frames=%llu cpu_frames=%llu\n",
            (unsigned long long)gpuFrames, (unsigned long long)cpuFrames);
    return 0;
}
}
int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& e) { fprintf(stderr, "S2 GPU helper: %s\n", e.what()); return 1; }
}
