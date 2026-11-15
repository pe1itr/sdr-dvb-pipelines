// Test-only GPU implementation: corrupts every output, then reports device loss.
// The helper must retry original LLRs and never attempt this device again.
#include "dect2/dsp/gpu_ldpc.h"
#include <algorithm>
#include <cstdio>
namespace dect2 {
struct GpuLdpc::Impl {};
GpuLdpc::GpuLdpc() : p_(new Impl) {}
GpuLdpc::~GpuLdpc() = default;
GpuLdpc& GpuLdpc::instance() { static GpuLdpc g; return g; }
bool GpuLdpc::available() const { return true; }
const char* GpuLdpc::deviceName() const { return "TEST device failure"; }
bool GpuLdpc::decode(const LdpcCode& code, const float*, int nb, int,
                     uint8_t* hard, uint8_t* ok, int* iterations, float, bool) {
    std::fill(hard, hard+nb*code.n(), 1);
    std::fill(ok, ok+nb, 1);
    std::fill(iterations, iterations+nb, 25);
    fprintf(stderr,"[s2-device-failure-test] failed after writing output\n");
    return false;
}
bool GpuLdpc::qcAvailable() const { return false; }
bool GpuLdpc::decodeQc(const QcLdpc&, const float*, int, int, uint8_t*, uint8_t*, int*, float, bool) { return false; }
bool GpuLdpc::boxCorrKernels() const { return false; }
}
