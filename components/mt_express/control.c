// SPDX-License-Identifier: GPL-3.0-only
// FTDI AN_108, MPSSE low-byte commands. SCL is AD0, SDA out/in AD1/AD2.
// Each transaction is clocked by a single USB command sequence. Only the
// two bus pins can go high; the AD4/5 auxiliary supply controls stay low.
// The upper bank is set once at open, with reset released and LNB enable low.
#include "control.h"
#include <stdbool.h>

enum { max_data = 16, sda_in = 4 };
typedef struct {
    uint8_t commands[32768];
    size_t used;
    unsigned replies;
} Transaction;

static void pins(Transaction *t, bool clock, bool release_data) {
    // Repeat levels for bus rise time on the Express pull-ups.
    for (int dwell=0; dwell<8; ++dwell) {
        t->commands[t->used++] = 0x80;
        t->commands[t->used++] = clock ? 1 : 0;
        t->commands[t->used++] = release_data ? 0x31 : 0x33;
    }
}
static void sample(Transaction *t) {
    t->commands[t->used++] = 0x81;
    ++t->replies;
}
static void start(Transaction *t) {
    pins(t, true, true);
    pins(t, true, false);
    pins(t, false, false);
}
static void bit(Transaction *t, bool value, bool read) {
    pins(t, false, value);
    pins(t, true, value);
    if (read) sample(t);
    pins(t, false, value);
}
static void send_byte(Transaction *t, unsigned value) {
    for (unsigned mask = 128; mask; mask >>= 1) bit(t, value & mask, false);
    bit(t, true, true);
}
static void address_register(Transaction *t, uint8_t address, uint16_t reg, int width) {
    start(t);
    send_byte(t, address << 1);
    if (width == 2) send_byte(t, reg >> 8);
    send_byte(t, reg);
}
static int finish(libusb_device_handle *dev, Transaction *t, unsigned acks, uint8_t *data, size_t count) {
    pins(t, false, false);
    pins(t, true, false);
    pins(t, true, true);
    t->commands[t->used++] = 0x87; // send immediate
    int n = 0;
    int rc = libusb_bulk_transfer(dev, 0x02, t->commands, t->used, &n, 300);
    if (rc || n != (int)t->used) return rc ? rc : LIBUSB_ERROR_IO;
    uint8_t reply[256], packet[512];
    unsigned done = 0;
    for (unsigned attempts = 0; done < t->replies && attempts < 12; ++attempts) {
        rc = libusb_bulk_transfer(dev, 0x81, packet, sizeof(packet), &n, 100);
        if (rc) return rc;
        if (n < 2 || n-2 > (int)(t->replies - done)) return LIBUSB_ERROR_IO;
        for (int i=2; i<n; ++i) reply[done++] = packet[i];
    }
    if (done != t->replies) return LIBUSB_ERROR_TIMEOUT;
    for (unsigned i=0; i<acks; ++i)
        if (reply[i] & sda_in) return LIBUSB_ERROR_PIPE;
    for (size_t i=0; i<count; ++i) {
        unsigned value=0;
        for (unsigned k=0; k<8; ++k) value=(value<<1) | !!(reply[acks++] & sda_in);
        data[i]=value;
    }
    return 0;
}
int mt_register_read(libusb_device_handle *dev, uint8_t address, uint16_t reg,
                     int reg_bytes, uint8_t *result, size_t count) {
    if (!count || count>max_data || (reg_bytes!=1 && reg_bytes!=2)) return LIBUSB_ERROR_INVALID_PARAM;
    Transaction t={0};
    address_register(&t,address,reg,reg_bytes);
    pins(&t,false,true);
    start(&t);
    send_byte(&t,(address<<1)|1);
    unsigned acks=t.replies;
    for (size_t i=0; i<count; ++i) {
        for (unsigned k=0; k<8; ++k) bit(&t,true,true);
        bit(&t,i+1==count,false);
    }
    return finish(dev,&t,acks,result,count);
}
int mt_register_write(libusb_device_handle *dev, uint8_t address, uint16_t reg,
                      int reg_bytes, const uint8_t *data, size_t count) {
    if (!count || count>max_data || (reg_bytes!=1 && reg_bytes!=2)) return LIBUSB_ERROR_INVALID_PARAM;
    Transaction t={0};
    address_register(&t,address,reg,reg_bytes);
    for (size_t i=0; i<count; ++i) send_byte(&t,data[i]);
    return finish(dev,&t,t.replies,NULL,0);
}

int mt_control_open(libusb_device_handle *dev) {
    int rc=libusb_claim_interface(dev,0);
    if (rc) return rc;
    rc=libusb_control_transfer(dev,0x40,0x0b,0x0200,1,NULL,0,300);
    uint8_t pins_off[]={0x82,0x01,0x91,0x80,0x01,0x31};
    int n=0;
    if (!rc) rc=libusb_bulk_transfer(dev,0x02,pins_off,sizeof(pins_off),&n,300);
    if (!rc && n!=(int)sizeof(pins_off)) rc=LIBUSB_ERROR_IO;
    uint8_t id[2];
    if (!rc) rc=mt_register_read(dev,0x69,0xf100,2,id,2);
    if (!rc && id[0]!=0x51) rc=LIBUSB_ERROR_NO_DEVICE;
    if (rc) libusb_release_interface(dev,0);
    return rc;
}

#include <stdio.h>
#include <time.h>
#include <string.h>
#define DEMOD 0x69
#define TUNER 0x60
static int demod_byte(libusb_device_handle *d, unsigned reg, uint8_t value) {
    return mt_register_write(d,DEMOD,reg,2,&value,1);
}
static int demod_word(libusb_device_handle *d, unsigned reg, uint16_t value) {
    uint8_t bytes[]={value >> 8,value & 255};
    return mt_register_write(d,DEMOD,reg,2,bytes,2);
}
int mt_restart_carrier(libusb_device_handle *d, uint16_t initial_offset) {
    int rc=demod_byte(d,0xf216,0x1c);
    if (!rc) rc=demod_word(d,0xf248,initial_offset);
    if (!rc) rc=demod_byte(d,0xf216,0x15);
    return rc;
}
static int tuner_access(libusb_device_handle *d, uint8_t gate, unsigned reg, uint8_t *data, size_t n, bool write) {
    int rc=demod_byte(d,0xf12a,gate|128);
    if (rc) return rc;
    return write ? mt_register_write(d,TUNER,reg,1,data,n) : mt_register_read(d,TUNER,reg,1,data,n);
}
// The demodulator PLL uses the module's 30 MHz reference and an output
// divider of four. Low SR uses input divider two and feedback divider eleven.
static void clock_dividers(unsigned symbol_rate, uint8_t *input, uint8_t *feedback) {
    if (symbol_rate<=125000) {
        *input=2;
        *feedback=11;
    } else {
        *input=1;
        *feedback=18;
    }
}
unsigned mt_master_clock_hz(unsigned symbol_rate) {
    uint8_t input,feedback;
    clock_dividers(symbol_rate,&input,&feedback);
    return 30000000U*feedback/(4U*input);
}

// Values below were read back from the local Express on PI6EHV, then
// compared with its hardware reset state. Only the required control fields
// are programmed; measurement/status registers are never replayed.
static int setup_demodulator(libusb_device_handle *d, unsigned symbol_rate) {
    uint8_t input,feedback;
    clock_dividers(symbol_rate,&input,&feedback);
    uint8_t clock[]={0x38|input,feedback,4,2};
    int rc=mt_register_write(d,DEMOD,0xf1b3,2,clock,sizeof(clock));
    for (unsigned i=0; !rc && i<50; ++i) {
        uint8_t locked;
        rc=mt_register_read(d,DEMOD,0xf1b8,2,&locked,1);
        if (rc || (locked&1)) break;
        if (i==49) return LIBUSB_ERROR_TIMEOUT;
        struct timespec delay={0,2000000}; nanosleep(&delay,NULL);
    }
    static const struct { uint16_t reg; uint8_t value; } setup[]={
        {0xf12a,0x38},{0xf12b,0x38},  // NIM I2C repeaters, initially closed
        {0xfa86,0x15},                 // FEC allocation for single transport
        {0xf11c,0x00},{0xf630,0x00},  // parallel TS output pins and routing
        {0xf370,0xf0},{0xf372,0x80},  // TS formatter and parallel port
        {0xf21f,0x04},{0xf253,0x1e},  // acquisition and timing lock
        {0xf25a,0x80},{0xf25b,0x80},  // timing loop
        {0xf23e,0x1c},{0xf297,0x0b},{0xf298,0x0a}, // carrier tracking
        {0xf29c,0x84},{0xf29d,0x84},{0xf29e,0x84},{0xf29f,0x84},
        {0xf333,0},{0xf30d,0x2c}
    };
    for (unsigned i=0; !rc && i<sizeof(setup)/sizeof(setup[0]); ++i)
        rc=demod_byte(d,setup[i].reg,setup[i].value);
    return rc;
}

static int setup_lnas(libusb_device_handle *d, uint8_t gate, bool input_b) {
    for (unsigned i=0;i<2;++i) {
        uint8_t addr=i ? 0x64 : 0x67;
        uint8_t path=i==input_b ? 0x50 : 0x88;
        int rc=demod_byte(d,0xf12a,gate|128);
        if (!rc) rc=mt_register_write(d,addr,2,1,&path,1);
        if (rc) return rc;
    }
    return 0;
}

// Request the current STVVGLNA gain on the selected RF input. The repeater
// closes after each downstream transaction. Restore the closed gate on exit.
int mt_lna_gain(libusb_device_handle *d, unsigned input_b, float *gain_db) {
    uint8_t addr=input_b ? 0x64 : 0x67;
    uint8_t gate,ctrl,values[3];
    int rc=mt_register_read(d,DEMOD,0xf12a,2,&gate,1);
    if (rc) return rc;
    rc=demod_byte(d,0xf12a,gate|128);
    if (!rc) rc=mt_register_read(d,addr,1,1,&ctrl,1);
    if (!rc) {
        ctrl=(ctrl & ~0x40)|0x20;
        rc=demod_byte(d,0xf12a,gate|128);
        if (!rc) rc=mt_register_write(d,addr,1,1,&ctrl,1);
    }
    if (!rc) {
        struct timespec delay={0,5000000};
        nanosleep(&delay,NULL);
        rc=demod_byte(d,0xf12a,gate|128);
        if (!rc) rc=mt_register_read(d,addr,1,1,values,sizeof(values));
    }
    int closed=demod_byte(d,0xf12a,gate&127);
    if (rc || closed) return rc ? rc : closed;
    // Endpoints derived from MiniTioune. See the provenance in README.md.
    // The fine-gain steps are nonlinear. This is an indication in dB.
    static const float low[]={-17.2f,-12.1f,-6.0f,1.85f};
    static const float high[]={-8.1f,-1.6f,4.85f,13.0f};
    unsigned stage=values[2]&3, fine=values[0]&31;
    float fraction=fine ? (fine-1)/30.0f : 0;
    *gain_db=low[stage]+fraction*(high[stage]-low[stage]);
    return 0;
}

// The tuner paths have the same synthesizer layout at different offsets.
static void pll_registers(unsigned khz, unsigned filter, uint8_t regs[5]) {
    unsigned divider=0;
    while (divider<3 && ((uint64_t)khz<<(divider+1))<2400000) ++divider;
    uint64_t ratio=(((uint64_t)khz<<(divider+19))+7500)/15000;
    unsigned integer=ratio>>18, fraction=ratio&0x3ffff;
    regs[0]=integer;
    regs[1]=((fraction&127)<<1)|(integer>>8);
    regs[2]=fraction>>7;
    regs[3]=(regs[3]&0x88)|0x10|(fraction>>15);
    regs[4]=(divider<<5)|filter;
}

static int tune_pll(libusb_device_handle *d, const MtTune *s, uint8_t gate) {
    // Serit FTS-4334L rev F1, section 3.2. Path 1 reference measured on
    // the Express is 30 MHz. RDIV divides by two when set.
    uint8_t regs[11];
    int rc=tuner_access(d,gate,0,regs,sizeof(regs),false);
    if (rc) return rc;
    uint8_t reference_and_gain[]={0x75,0x30};
    rc=tuner_access(d,gate,0,reference_and_gain,sizeof(reference_and_gain),true);
    if (rc) return rc;
    // The baseband filter has a 5 MHz minimum cutoff and 1 MHz steps.
    unsigned cutoff=(s->symbol_rate*7ULL+9999999)/10000000;
    pll_registers(s->frequency_khz,cutoff>5 ? cutoff-5 : 0,regs+2);
    rc=tuner_access(d,gate,2,regs+2,5,true);
    if (rc) return rc;
    // The Express B connection measured RF-C on path 1; A uses RF-B.
    regs[9]=(regs[9]&0xfc)|(s->input_b ? 2 : 1);
    regs[10]=(regs[10]&0xc2)|1|(s->input_b ? 0x10 : 0x08);
    rc=tuner_access(d,gate,9,regs+9,2,true);
    if (rc) return rc;
    uint8_t cal=6;
    rc=tuner_access(d,gate,8,&cal,1,true);
    for (unsigned i=0; !rc && i<40; ++i) {
        struct timespec delay={0,5000000}; nanosleep(&delay,NULL);
        rc=tuner_access(d,gate,8,&cal,1,false);
        if (!rc && (cal&7)==1) {
            uint8_t verify[5];
            rc=tuner_access(d,gate,2,verify,sizeof(verify),false);
            if (!rc && memcmp(verify,regs+2,5)) rc=LIBUSB_ERROR_IO;
            return rc;
        }
    }
    return rc ? rc : LIBUSB_ERROR_TIMEOUT;
}
int mt_tune(libusb_device_handle *d, const MtTune *s) {
    if(s->frequency_khz<144000 || s->frequency_khz>2450000 ||
       s->symbol_rate<70000 || s->symbol_rate>10000000 ||
       (s->standard!=1 && s->standard!=2) || s->input_b>1 ||
       s->code>(s->standard==1 ? 63U : 28U) || s->search_khz>30000 || s->detector1>2 || s->detector2>2)
        return LIBUSB_ERROR_INVALID_PARAM;
    uint8_t gate=0;
    int rc=setup_demodulator(d,s->symbol_rate);
    if (rc) return rc;
    rc=mt_register_read(d,DEMOD,0xf12a,2,&gate,1);
    if (rc) return rc;
    for (unsigned attempt=0; attempt<3; ++attempt) {
        rc=tune_pll(d,s,gate);
        if (rc!=LIBUSB_ERROR_TIMEOUT) break;
    }
    if (!rc) rc=setup_lnas(d,gate,s->input_b);
    int close_rc=demod_byte(d,0xf12a,gate&127);
    if (rc || close_rc) return rc ? rc : close_rc;
    uint8_t mode;
    rc=mt_register_read(d,DEMOD,0xf214,2,&mode,1);
    if (!rc) rc=demod_byte(d,0xf216,0x1c); // stop acquisition for parameter update
    const unsigned registers[]={0xf238,0xf290};
    const unsigned detectors[]={s->detector1,s->detector2};
    for (unsigned i=0;!rc && i<2;i++) {
        uint8_t config;
        rc=mt_register_read(d,DEMOD,registers[i],2,&config,1);
        if (!rc) rc=demod_byte(d,registers[i],(config&~3)|detectors[i]);
    }
    // Carrier gains are raw mantissa/exponent codes. Keep the measured
    // mode bits and restore the measured defaults on every acquisition.
    static const uint8_t alpha[]={0x0b,0x0a,0x49,0x48};
    if (!rc) rc=demod_byte(d,0xf239,s->alpha<0 ? 0x2b : s->alpha);
    if (!rc) rc=demod_byte(d,0xf23a,s->beta<0 ? 0x1a : s->beta);
    for (unsigned m=0;!rc && m<4;++m) {
        rc=demod_byte(d,0xf297+m,s->alpha<0 ? alpha[m] : (alpha[m]&0xc0)|s->alpha);
        if (!rc) rc=demod_byte(d,0xf29c+m,s->beta<0 ? 0x84 : 0x80|s->beta);
    }
    if (!rc) rc=demod_byte(d,0xf23f,s->lock_acquire<0 ? 0xd0 : s->lock_acquire);
    if (!rc) rc=demod_byte(d,0xf240,s->lock_release<0 ? 0xb8 : s->lock_release);
    if (!rc) rc=demod_byte(d,0xf214,(mode&0x3f)|(s->standard==2 ? 0x80 : 0x40));
    if (!rc) rc=demod_byte(d,0xf211,s->standard==2 && s->code ? 0x80|(s->code<<2) : 0);
    if (!rc) rc=demod_byte(d,0xf33c,s->standard==1 && s->code ? s->code : 0x3f);
    unsigned master=mt_master_clock_hz(s->symbol_rate);
    unsigned sr=(s->symbol_rate*65536ULL+master/2)/master;
    unsigned window=(s->search_khz ? s->search_khz : s->symbol_rate*3/2000)*65536ULL/(master/1000);
    // The Express accepted a maximum positive carrier bound of 0x3fff on readback.
    if (window>0x3fff) window=0x3fff;
    if(!rc) rc=demod_word(d,0xf25e,sr);
    if(!rc) rc=demod_word(d,0xf260,sr+sr/50+1);
    if(!rc) rc=demod_word(d,0xf262,sr-sr/50-1);
    if(!rc) rc=demod_word(d,0xf242,window);
    if(!rc) rc=demod_word(d,0xf246,-(int)window);
    if(!rc) rc=demod_word(d,0xf248,0);
    if(!rc) rc=demod_byte(d,0xf216,0x15);
    return rc;
}

// STV6120 register map, Serit FTS-4334L rev F1 section 3.2. Only the
// path-2 fields change. Leave the common reference and input LNAs alone.
int mt_scan_restore(libusb_device_handle *dev, MtScanPath *path) {
    if (!path->active) return 0;
    int rc=tuner_access(dev,path->gate,9,path->saved,sizeof(path->saved),true);
    if (!rc && (path->saved[1]&2) && (path->saved[2]&0x20)) {
        uint8_t calibrate=6;
        rc=tuner_access(dev,path->gate,18,&calibrate,1,true);
    }
    int closed=demod_byte(dev,0xf12a,path->gate);
    if (!rc && !closed) path->active=0;
    return rc ? rc : closed;
}

int mt_scan_power(libusb_device_handle *dev, MtScanPath *path, unsigned khz, uint16_t *power) {
    *power=UINT16_MAX;
    if (khz<144000 || khz>2450000) return 0;
    int rc=0;
    if (!path->active) {
        rc=mt_register_read(dev,DEMOD,0xf12a,2,&path->gate,1);
        if (rc) return rc;
        rc=tuner_access(dev,path->gate,9,path->saved,sizeof(path->saved),false);
        if (rc) {
            demod_byte(dev,0xf12a,path->gate);
            return rc;
        }
        uint8_t input=path->saved[0]&3;
        // CTRL11: SYN_2 (bit 5) on, SDoff_2 (bit 6) off.
        // Preserve REFOUT_SEL_2 (bit 4), the DC loop and baseband gain.
        uint8_t route[]={(path->saved[0]&~12)|(input<<2),path->saved[1]|2,
                         (path->saved[2]&~0x40)|0x20};
        path->active=1;
        rc=tuner_access(dev,path->gate,9,route,sizeof(route),true);
    }
    uint8_t pll[5]={0,0,0,path->saved[6],0};
    pll_registers(khz,0,pll);
    if (!rc) rc=tuner_access(dev,path->gate,12,pll,sizeof(pll),true);
    uint8_t calibration=0;
    for (int attempt=0; !rc && attempt<2; ++attempt) {
        calibration=6;
        rc=tuner_access(dev,path->gate,18,&calibration,1,true);
        for (int poll=0; !rc && poll<20; ++poll) {
            struct timespec delay={0,5000000};
            nanosleep(&delay,NULL);
            rc=tuner_access(dev,path->gate,18,&calibration,1,false);
            if ((calibration&7)==1) break;
        }
        if ((calibration&7)==1) break;
    }
    int closed=demod_byte(dev,0xf12a,path->gate);
    if (rc || closed) return rc ? rc : closed;
    if ((calibration&7)!=1) return 0;
    uint8_t iq[2];
    rc=mt_register_read(dev,DEMOD,0xf40a,2,iq,sizeof(iq));
    if (!rc) *power=iq[0]+iq[1];
    return rc;
}
