// Inject a GPU failure after availability, without needing GPU hardware.
#include "dect2/dsp/gpu_ldpc.h"
#include "dect2/t2/t2plp.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
static int initialized=0, calls=0;
namespace dect2 {
struct GpuLdpc::Impl {};
GpuLdpc::GpuLdpc() : p_(new Impl) { ++initialized; }
GpuLdpc::~GpuLdpc() = default;
GpuLdpc& GpuLdpc::instance() { static GpuLdpc g; return g; }
bool GpuLdpc::available() const { return true; }
const char* GpuLdpc::deviceName() const { return "injected failure"; }
#ifdef _WIN32
size_t GpuLdpc::dedicatedMemoryMb() const { return 2048; }
#endif
bool GpuLdpc::decode(const LdpcCode&,const float*,int,int,uint8_t*,uint8_t*,int*,float,bool) { ++calls; return false; }
}
static PlpJob job() {
    PlpJob j; j.fec.shortFrame=true; j.fec.mod=0; j.numBlocks=1; j.frameSec=1;
    auto d=fecDims(j.fec);
    std::vector<uint8_t> bits(d.kBch,0);
    BbHeader h; h.dfl=d.kBch-80; buildBbHeader(h,bits.data());
    for (int i=0;i<d.kBch;++i) bits[i]^=bbRandomiser()[i];
    bchFor(j.fec).encode(bits,d.kBch); ldpcFor(j.fec).encode(bits);
    const auto& map=bitInterleaverMap(j.fec);
    std::vector<uint16_t> labels(d.cellsPerBlock);
    for (int c=0;c<d.cellsPerBlock;++c) for (int b=0;b<d.bitsPerCell;++b)
        labels[c]=(labels[c]<<1)|bits[map[c*d.bitsPerCell+b]];
    std::vector<cf32> mapped; qamMapBlock(j.fec,labels,mapped);
    cellInterleave(j.fec,1,0,mapped,j.cells); j.n0.assign(j.cells.size(),.01f);
    return j;
}
static PlpResult receive(PlpDecoder& decoder) {
    PlpResult r;
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while (!decoder.poll(r)) {
        assert(std::chrono::steady_clock::now()<end);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return r;
}
int main() {
    PlpDecoder decoder(1);
    decoder.setMode(0);
    auto input=job(); auto reference=PlpDecoder::decodeNow(input,1,false);
    assert(initialized==0 && calls==0 && reference.blocksOk==1 && reference.headerOk==1);
    decoder.setMode(1); assert(initialized==1);
    for (int i=0;i<2;++i) {
        auto copy=input; assert(decoder.submit(std::move(copy)));
        auto r=receive(decoder);
        assert(!r.usedGpu && r.blocksOk==1 && r.headerOk==1);
        assert(r.frames[0].bits==reference.frames[0].bits);
    }
    assert(calls==1 && !decoder.gpuAvailable()); // failure latches for this run
    decoder.setMode(0);
    assert(initialized==1 && calls==1);
    puts("GPU fallback: CPU stays lazy; failed GPU frame recovered exactly; later frames stay on CPU");
}
