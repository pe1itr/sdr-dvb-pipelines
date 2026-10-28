/* Exercise the real streaming Viterbi implementation with independently encoded
 * DVB mother-code bits, puncturing, errors, and arbitrary whole-symbol chunks. */
#include "../third_party/rbdvbt/src/probe_constellation.c"
#include <assert.h>

static unsigned parity(unsigned v) {
    unsigned p=0;
    while (v) { p^=v&1; v>>=1; }
    return p;
}

static void check(rbdvbt_fec_t fec, int errors) {
    /* Whole puncturing periods for every rate, including the final chunk. */
    enum { BYTES=22680, MAX_BITS=BYTES*16 };
    uint8_t input[BYTES], bits[MAX_BITS], output[BYTES];
    uint32_t random=1234567;
    for (unsigned i=0;i<BYTES;++i) {
        random=1664525u*random+1013904223u;
        input[i]=i<32 ? 0 : random>>24;
    }
    unsigned reg=0, n=0;
    /* Independent encoder masks from DVB BlueBook A012, section 4.3.3/table 2. */
    const char *x, *y;
    switch (fec) {
    case RBDVBT_FEC_1_2: x="1"; y="1"; break;
    case RBDVBT_FEC_2_3: x="10"; y="11"; break;
    case RBDVBT_FEC_3_4: x="101"; y="110"; break;
    case RBDVBT_FEC_5_6: x="10101"; y="11010"; break;
    case RBDVBT_FEC_7_8: x="1000101"; y="1111010"; break;
    default: abort();
    }
    unsigned period=strlen(x);
    for (unsigned i=0;i<BYTES*8;++i) {
        reg=(reg>>1) | (((input[i/8]>>(7-i%8))&1)<<6);
        unsigned phase=i%period;
        if (x[phase]=='1') bits[n++]=parity(reg & 0171);
        if (y[phase]=='1') bits[n++]=parity(reg & 0133);
    }
    if (errors) for (unsigned i=1001;i<n;i+=2003) bits[i]^=1;
    qpsk_dibit_t *dibits=calloc(n/2,sizeof(*dibits)); assert(dibits);
    for (unsigned i=0;i<n/2;++i) {
        dibits[i].dibit=bits[2*i] | (bits[2*i+1]<<1);
        dibits[i].x_cost[0]=bits[2*i] ? 1 : 0;
        dibits[i].x_cost[1]=bits[2*i] ? 0 : 1;
        dibits[i].y_cost[0]=bits[2*i+1] ? 1 : 0;
        dibits[i].y_cost[1]=bits[2*i+1] ? 0 : 1;
    }
    assert(live_viterbi_stream_supported(fec));
    for (unsigned split=0;split<2;++split) {
        live_viterbi_stream_reset();
        size_t position=0,total=0;
        for (unsigned chunk=0;position<n/2;++chunk) {
            size_t count=split ? 1512*(1+chunk%3) : n/2;
            if (count>n/2-position) count=n/2-position;
            uint8_t *decoded=NULL; size_t decoded_size=0;
            assert(!live_viterbi_stream_decode_bytes(fec,dibits+position,count,position!=0,
                &decoded,&decoded_size,NULL,NULL,NULL));
            assert(total+decoded_size<=sizeof(output));
            memcpy(output+total,decoded,decoded_size); total+=decoded_size; position+=count;
        }
        assert(total==BYTES-viterbi_traceback_bytes(fec));
        assert(!memcmp(output,input,total));
    }
    free(dibits);
}

int main(void) {
    const rbdvbt_fec_t rates[]={RBDVBT_FEC_1_2,RBDVBT_FEC_2_3,RBDVBT_FEC_3_4,RBDVBT_FEC_5_6,RBDVBT_FEC_7_8};
    for (unsigned i=0;i<sizeof(rates)/sizeof(rates[0]);++i)
        for (int errors=0;errors<2;++errors) check(rates[i],errors);
    puts("DVB-T streaming FEC 1/2, 2/3, 3/4, 5/6, 7/8: exact bytes, puncturing, errors, chunk continuity and reset passed");
    return 0;
}
