// Deterministic nonzero codewords and the pre-optimization batch policy.
// Encoding/parity validation use the independent accumulator encoder, while
// reference decoding retains scalar single frames and full-width batches.
#define main bundled_ldpc_main
#include "../third_party/ldpc/ldpc_tool.cc"
#undef main
#include "encoder.hh"
#include <memory>

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    const std::string action = argv[1], table = argv[2];
    if (table.size() < 2) return 2;
    std::unique_ptr<LDPCInterface> ldpc(create_ldpc((char *)"S2", table[0], atoi(table.c_str()+1)));
    if (!ldpc) return 2;
    const int n = ldpc->code_len(), k = ldpc->data_len();
    LDPCEncoder<int8_t> encoder;
    encoder.init(ldpc.get());
    std::vector<int8_t> code(32*n), hard(n);
    if (action == "generate") {
        std::mt19937 rng(731 + table[0]*100 + atoi(table.c_str()+1));
        for (int lane=0; lane<32; ++lane) {
            int8_t *word = code.data()+lane*n;
            for (int bit=0; bit<k; ++bit) word[bit] = (rng() & 1) ? -1 : 1;
            encoder(word, word+k);
            for (int bit=0; bit<n; ++bit) word[bit] *= 40;
        }
        return fwrite(code.data(),1,code.size(),stdout)==code.size() ? 0 : 1;
    }
    if (action == "reference") {
        DecoderRunner<simd_type> wide;
        DecoderRunner<SIMD<int8_t,1>> narrow;
        uint32_t count;
        while (read_full(&count,sizeof(count),true)) {
            if (!count || count>32) return 2;
            read_full(code.data(),size_t(count)*n,false);
            if (count==1) narrow.run(code.data(),count,ldpc.get());
            else wide.run(code.data(),count,ldpc.get());
            if (fwrite(code.data(),n,count,stdout)!=count) return 1;
        }
        return 0;
    }
    if (action == "verify") {
        while (read_full(code.data(),n,true)) {
            for (int bit=0; bit<k; ++bit) hard[bit] = code[bit]<0 ? -1 : 1;
            encoder(hard.data(),hard.data()+k);
            uint8_t valid = 1;
            for (int bit=k; bit<n; ++bit)
                if ((code[bit]<0) != (hard[bit]<0)) valid=0;
            if (fwrite(&valid,1,1,stdout)!=1) return 1;
        }
        return 0;
    }
    return 2;
}
