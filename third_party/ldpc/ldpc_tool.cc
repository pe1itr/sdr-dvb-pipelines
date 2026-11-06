#include "../../src/aligned_memory.h"
/*
LDPC testbench
Copyright 2018 Ahmet Inan <xdsopl@gmail.com>

Transformed into external decoder for third-party applications
Copyright 2019 <pabr@pabr.org>
*/

#include <stdlib.h>
#include <cerrno>
#include <string>
#include <vector>
#include <limits.h>
#include <unistd.h>
#include <iostream>
#include <iomanip>
#include <random>
#include <cmath>
#include <cassert>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <functional>
#include "testbench.hh"
#ifdef __AVX2__
// AVX2 CPUs also support SSE4.1. Use its native saturating arithmetic for
// small batches, including one actual frame, without waiting for more frames.
#include "sse4_1.hh"
#endif
#include "algorithms.hh"

#if 0
#include "flooding_decoder.hh"
static const int TRIALS = 50;
#else
#include "layered_decoder.hh"
static const int TRIALS = 25;
#endif

LDPCInterface *create_ldpc(char *standard, char prefix, int number);

void fail(const char *msg) {
	std::cerr << "** plugin: " << msg << std::endl;
	exit(1);
}
void fatal(const char *msg) {
	fprintf(stderr, "** plugin: ");
	perror(msg);
	exit(1);
}

void usage(const char *name, FILE *f, int c, const char *info=NULL) {
    fprintf(f, "Usage: %s --modcod INT [options] < FECFRAMES.int8 > FECFRAMES.int8\n", name);
    fprintf(f,
        "DVB-S2 soft-decision LDPC helper; input/output are int8 LLR frames, not IQ or TS.\n"
        "  --standard DVB-S2  Only supported standard (default)\n"
        "  --modcod INT       DVB-S2 MODCOD; must support the selected frame size\n"
        "  --shortframes      16200-bit frames (default: 64800-bit normal frames)\n"
        "  --framed           Local protocol: native uint32 batch length (1..32), then LLR frames\n"
        "                     Output contains the same number of frames, without a length header\n"
        "                     Without --framed, raw input is processed in batches of 32\n"
        "  --version          Show build revision and selected SIMD width\n"
        "  -h, --help         Show this help\n"
        "AVX2 is selected automatically when supported and the companion binary is present.\n"
        "The AVX2 build uses 16 SIMD lanes for batches of 1..16, without extra batching.\n"
        "Set LINRAD_LDPC_PORTABLE=1 to force the portable build when invoking ldpc_tool.\n");
	if ( info ) fprintf(f, "** Error while processing '%s'\n", info);
	exit(c);
}

template<typename V> struct DecoderRunner {
    typedef MinSumCAlgorithm<V, NormalUpdate<V>, FACTOR> Algorithm;
    LDPCDecoder<V, Algorithm> decoder;
    V *simd = nullptr;
    int length = 0, data_length = 0;
    void run(int8_t *code, int blocks, LDPCInterface *ldpc) {
        if (!simd) {
            length = ldpc->code_len(); data_length = ldpc->data_len();
            decoder.init(ldpc);
            simd = static_cast<V *>(linrad_aligned_alloc(64, (sizeof(V)*length+63)&~size_t(63)));
            if (!simd) fatal("allocate LDPC workspace");
        }
        for (int j=0; j<blocks; j+=V::SIZE) {
            int count = std::min(V::SIZE, blocks-j);
            memset(simd, 0, sizeof(V)*length);
            for (int n=0; n<count; ++n)
                for (int i=0; i<length; ++i) simd[i].v[n] = code[(j+n)*length+i];
            decoder(simd, simd+data_length, TRIALS, count);
            for (int n=0; n<count; ++n)
                for (int i=0; i<length; ++i) code[(j+n)*length+i] = simd[i].v[n];
        }
    }
    ~DecoderRunner() { linrad_aligned_free(simd); }
};

// EOF is allowed only between records; truncated frames are errors.
static bool read_full(void *buffer, size_t bytes, bool eof_ok) {
    char *p = static_cast<char *>(buffer); size_t pos=0;
    while (pos<bytes) {
        ssize_t n=read(0,p+pos,bytes-pos);
        if (n<0 && errno==EINTR) continue;
        if (n<0) fatal("read");
        if (!n) { if (!pos && eof_ok) return false; fail("truncated input frame"); }
        pos+=n;
    }
    return true;
}

int main(int argc, char **argv)
{
	#if defined(__x86_64__) && !defined(__AVX2__)
    if (__builtin_cpu_supports("avx2") && !getenv("LINRAD_LDPC_PORTABLE")) {
        char path[PATH_MAX]; ssize_t n=readlink("/proc/self/exe",path,sizeof(path)-1);
        if (n>0) { path[n]=0; std::string fast=std::string(path)+"_avx2";
            execv(fast.c_str(),argv); } // Portable fallback if companion absent.
    }
#endif
    const char *standard = "DVB-S2";
    bool framed = false;
	int modcod = -1;
	bool shortframes = false;
	for ( int i=1; i<argc; ++i ) {
		if      ( ! strcmp(argv[i],"--standard") && i+1<argc ) standard = argv[++i];
		else if ( ! strcmp(argv[i],"--modcod") && i+1<argc )   modcod = atoi(argv[++i]);
		else if ( ! strcmp(argv[i],"--framed") ) framed = true;
        else if ( ! strcmp(argv[i],"--version") ) {
#ifdef __AVX2__
            const int small_width = 16;
#else
            const int small_width = 1;
#endif
            printf("linrad LDPC 6ada4aa SIMD=%d low-latency=1 small-batch-simd=%d\n",
                   SIMD_WIDTH, small_width); return 0;
        }
        else if ( ! strcmp(argv[i],"--shortframes") )          shortframes = true;
		else if ( ! strcmp(argv[i],"-h") || !strcmp(argv[i],"--help") )                     usage(argv[0], stdout, 0);
		else                                                   usage(argv[0], stderr, 1, argv[i]);
	}

	if ( strcmp(standard, "DVB-S2") ) fail("Only DVB-S2 is supported.");
	if ( modcod<0 || modcod>31 ) usage(argv[0], stderr, 1);

	typedef NormalUpdate<simd_type> update_type;
	//typedef SelfCorrectedUpdate<simd_type> update_type;

	//typedef MinSumAlgorithm<simd_type, update_type> algorithm_type;
	//typedef OffsetMinSumAlgorithm<simd_type, update_type, FACTOR> algorithm_type;
	typedef MinSumCAlgorithm<simd_type, update_type, FACTOR> algorithm_type;
	//typedef LogDomainSPA<simd_type, update_type> algorithm_type;
	//typedef LambdaMinAlgorithm<simd_type, update_type, 3> algorithm_type;
	//typedef SumProductAlgorithm<simd_type, update_type> algorithm_type;

	LDPCDecoder<simd_type, algorithm_type> decode;

	// DVB-S2 MODCOD definitions
	static const char *mc_tabnames[2][32] = {  // [shortframes][modcod]
	  { // Normal frames
	    0,"B1","B2","B3","B4","B5","B6","B7",
	    "B8","B9","B10","B11","B5","B6","B7","B9",
	    "B10","B11","B6","B7","B8","B9","B10","B11",
	    "B7","B8","B8","B10","B11",0,0,0
	  },
	  { // Short frames
	    0,"C1","C2","C3","C4","C5","C6","C7",
	    "C8","C9","C10",0,"C5","C6","C7","C9",
	    "C10",0,"C6","C7","C8","C9","C10",0,
	    "C7","C8","C8","C10",0,0,0,0
	  }
	};

	const char *tabname = mc_tabnames[shortframes][modcod];
	if ( ! tabname ) fail("unsupported modcod");
	LDPCInterface *ldpc = create_ldpc((char*)"S2", tabname[0], atoi(tabname+1));
	if (!ldpc) {
		std::cerr << "no such table!" << std::endl;
		return -1;
	}
	const int CODE_LEN = ldpc->code_len();
	const int DATA_LEN = ldpc->data_len();


    DecoderRunner<simd_type> wide;
#ifdef __AVX2__
    DecoderRunner<SIMD<int8_t,16>> small;
#else
    DecoderRunner<SIMD<int8_t,1>> narrow;
#endif
    std::vector<int8_t> code(32*CODE_LEN);
    for (;;) {
        uint32_t blocks=32;
        if (framed) {
            if (!read_full(&blocks,sizeof(blocks),true)) break;
            if (!blocks || blocks>32) fail("invalid batch size");
        }
        size_t bytes=size_t(blocks)*CODE_LEN;
        if (!read_full(code.data(),bytes,!framed)) break;
#ifdef __AVX2__
        // Keep the supplied batch intact: the convergence check covers every
        // active frame. Splitting larger batches would change stopping points.
        if (blocks<=16) small.run(code.data(),blocks,ldpc);
#else
        if (blocks==1) narrow.run(code.data(),blocks,ldpc);
#endif
        else wide.run(code.data(),blocks,ldpc);
        size_t pos=0;
        while (pos<bytes) {
            ssize_t n=write(1,code.data()+pos,bytes-pos);
            if (n<0 && errno==EINTR) continue;
            if (n<=0) fatal("write");
            pos+=n;
        }
    }
    delete ldpc;
    return 0;
}
