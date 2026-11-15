/* Small complete v1 transmission for the receiver adapter integration tests. */
#include "dvb_bond.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void psi(unsigned char p[188], unsigned pid, unsigned char *s, unsigned n) {
    unsigned crc=0xffffffffu;
    for (unsigned i=0;i<n;++i) {
        crc^=(unsigned)s[i]<<24;
        for (unsigned j=0;j<8;++j) crc=(crc<<1)^(0x04c11db7u & (0u-(crc>>31)));
    }
    for (unsigned j=0;j<4;++j) s[n+j]=(unsigned char)(crc>>(24-8*j));
    memset(p,0xff,188);p[0]=0x47;p[1]=0x40|(pid>>8);p[2]=pid;p[3]=0x10;p[4]=0;
    memcpy(p+5,s,n+4);
}

static unsigned source_cc[8192];
static void native_packet(unsigned char p[188],unsigned i) {
    if (i%50==0) {
        unsigned char pat[16]={0,0xb0,13,0,1,0xc1,0,0,0,1,0xf0,0};
        psi(p,0,pat,12);
    } else if (i%50==1) {
        unsigned char pmt[26]={2,0xb0,23,0,1,0xc1,0,0,0xe1,0,0xf0,0,
                              0x1b,0xe1,0,0xf0,0,3,0xe1,1,0xf0,0};
        psi(p,4096,pmt,22);
    } else {
        unsigned pid=i%3==2?256:i%5==4?257:8191;
        memset(p,0xff,188);p[0]=0x47;p[1]=pid>>8;p[2]=pid;
        if (pid!=8191) {
            p[1]|=0x40;p[3]=0x30;p[4]=23;p[5]=pid==256?0x10:0;
            if (pid==256) {
                uint64_t base=(uint64_t)i*1504*90000/50100;
                p[6]=base>>25;p[7]=base>>17;p[8]=base>>9;p[9]=base>>1;
                p[10]=((base&1)<<7)|0x7e;p[11]=0;
            }
            for (unsigned j=28;j<188;++j) p[j]=(unsigned char)(i*7+j);
            p[28]=p[29]=0;p[30]=1;p[31]=pid==256?0xe0:0xc0;
        } else p[3]=0x10;
    }
    unsigned pid=((p[1]&31u)<<8)|p[2];
    p[3]=(p[3]&0xf0)|(source_cc[pid]++&15);
}

int main(int argc,char **argv) {
    assert(argc==4 || (argc==5 && !strcmp(argv[4],"native")));
    FILE *src=fopen(argv[1],"wb"),*a=fopen(argv[2],"wb"),*b=fopen(argv[3],"wb");assert(src&&a&&b);
    dvb_bond_config_v1 c;dvb_bond_config_init_v1(&c);c.session=1234567;
    if (argc==5) c.distribution=DVB_BOND_DISTRIBUTION_SEPARATE_LANES;
    dvb_bond_tx *tx=dvb_bond_tx_create_v1(&c);assert(tx);
    unsigned char p[188];
    for (unsigned i=0;i<224;++i) {
        for (unsigned j=0;j<188;++j) p[j]=(unsigned char)(i*17+j);
        p[0]=0x47;p[1]=1;p[2]=0;p[3]=0x10|(i&15);
        if (argc==5) native_packet(p,i);
        assert(fwrite(p,1,188,src)==188);assert(!dvb_bond_tx_push(tx,p));
    }
    assert(!dvb_bond_tx_finish(tx));int ended[2]={0,0};
    for (uint64_t now=0;!ended[0]||!ended[1];now+=1000000) {
        assert(now<UINT64_C(60000000000));
        for (unsigned i=0;i<2;++i) if (!ended[i]) {
            int rc=dvb_bond_tx_pop(tx,i,now,p);assert(rc>=0);
            if (rc==DVB_BOND_END) ended[i]=1;
            if (rc==DVB_BOND_PACKET) assert(fwrite(p,1,188,i?b:a)==188);
        }
    }
    dvb_bond_tx_destroy(tx);fclose(src);fclose(a);fclose(b);return 0;
}
