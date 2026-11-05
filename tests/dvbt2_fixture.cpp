// File-only broadcast fixture; no SDR or transmit API. GPL-3.0.
#include "dect2/t2/t2gen.h"
#include "dect2/dsp/resampler.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace dect2;
int main(int argc, char** argv) {
    if (argc != 4) { fprintf(stderr,"usage: fixture output.iq 8k|32k s16|u8\n"); return 2; }
    const bool large = !strcmp(argv[2],"32k"), byte = !strcmp(argv[3],"u8");
    TxParams p; p.payload=true; p.s2field1=large ? 5 : 1;
    p.giIdx=large ? 6 : 2; p.ext=large; p.pp=large ? 6 : 0;
    p.plpMod=large ? 3 : 1; p.plpCod=large ? 1 : 2; p.plpTi=3; p.plpId=7;
    uint32_t packet=0;
    p.tsSource=[&](uint8_t* out) {
        out[0]=0x47; out[1]=1; out[2]=0; out[3]=0x10 | (packet & 15);
        for (int j=0;j<4;++j) out[4+j]=(packet >> (8*j)) & 255;
        for (int j=8;j<188;++j) out[j]=(packet*31u+j*17u) & 255;
        ++packet;
    };
    T2Generator generator(p);
    RationalResampler resampler;
    if (!resampler.configure(nativeRateHz(8),10000000)) return 1;
    FILE* file=fopen(argv[1],"wb"); if (!file) return 1;
    uint64_t count=0; std::vector<cf32> native, iq;
    double phase=0;
    for (int f=0;f<64;++f) {
        generator.nextFrame(native); iq.clear();
        resampler.process(native.data(),native.size(),iq);
        for (auto v : iq) {
            // A small receiver frequency offset also exercises CFO tracking.
            v *= std::polar(0.12f,(float)phase);
            phase = fmod(phase+2*M_PI*125/10000000,2*M_PI);
            float values[]={v.real(),v.imag()};
            for (float value : values) {
                if (byte) { uint8_t q=std::clamp((int)std::lround(127.5+128*value),0,255); if (fwrite(&q,1,1,file)!=1) return 1; }
                else { int16_t q=std::clamp((int)std::lround(32768*value),-32768,32767); if (fwrite(&q,2,1,file)!=1) return 1; }
            }
        }
        count += iq.size();
    }
    if (fclose(file)) return 1;
    fprintf(stderr,"fixture: %llu samples, 10 Msps, %s PLP 7, %u known TS packets\n",(unsigned long long)count,argv[2],packet);
}
