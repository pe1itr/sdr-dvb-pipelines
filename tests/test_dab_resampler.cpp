#include "dab_resampler.hpp"
#include <cassert>
#include <iostream>
using IQ=std::vector<std::complex<float>>;
static IQ run(unsigned rate,double frequency,size_t block) {
    DabResampler resampler(rate);IQ output;
    size_t count=rate/50;
    for (size_t offset=0;offset<count;) {
        size_t n=std::min(block,count-offset);IQ input(n);
        for (size_t i=0;i<n;++i) input[i]=std::polar(1.0f,float(2*M_PI*frequency*(offset+i)/rate));
        resampler.process(input);output.insert(output.end(),input.begin(),input.end());offset+=n;
    }
    return output;
}
static double power(const IQ& samples) {
    double sum=0;for (size_t i=2048;i<samples.size();++i) sum+=std::norm(samples[i]);
    return sum/(samples.size()-2048);
}
int main() {
    for (unsigned rate:{2048000u,2500000u,4000000u,4096000u,8000000u,8192000u,10000000u,12345678u,20000000u,61440000u}) {
        auto tone=run(rate,720000,65536),chunked=run(rate,720000,997);
        assert(tone.size()==chunked.size());// A polyphase converter retains up to two 1024-output periods;
        // this is fixed latency, not a continuing sample-rate mismatch.
        assert(tone.size()>40960-2100 && tone.size()<=40961);
        for (size_t i=0;i<tone.size();++i) assert(std::abs(tone[i]-chunked[i])<1e-5);
        assert(power(tone)>.97 && power(tone)<1.03);
        assert(power(run(rate,768000,65536))>.97);
        std::complex<double> phase{};
        for (size_t i=2049;i<tone.size();++i) phase+=std::complex<double>(tone[i]*std::conj(tone[i-1]));
        assert(std::abs(std::arg(phase)*2048000/(2*M_PI)-720000)<3);
        if (rate>=4000000) assert(power(run(rate,1300000,65536))<1e-5);
        DabResampler reset(rate);IQ zero(65536);reset.process(zero);reset.reset();
        zero.assign(65536,{});reset.process(zero);for(auto p:zero) assert(std::abs(p)<1e-12);
        std::cout<<rate<<": continuity, bandwidth, frequency and alias rejection passed\n";
    }
}
