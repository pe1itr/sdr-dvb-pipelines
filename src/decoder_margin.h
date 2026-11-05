#ifndef DECODER_MARGIN_H
#define DECODER_MARGIN_H
#include "decoder_monitor.h"
#include <math.h>
#include <string.h>

/* Reference MER reserve, not a calibrated receiver threshold. In AWGN,
 * MER approximates Es/N0; distortion and estimator bias affect this estimate.
 * DVB-S2: ETSI EN 302 307-1 V1.4.1, section 6, table 13:
 * https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf
 * The table assumes ideal synchronisation and 50 LDPC iterations, unlike our
 * receiver. For short frames use the conservative end of its 0.2..0.3 dB
 * allowance. This is an approximation, not a separate measured short table.
 * DVB-S: EN 300 421 V1.1.2, table 3 (includes implementation allowance):
 * https://www.etsi.org/deliver/etsi_en/300400_300499/300421/01.01.02_60/en_300421v010102p.pdf
 * Convert useful-bit Eb/N0 to Es/N0 with 10 log10(2 * FEC * 188/204).
 */
static inline double decoder_margin_reference(const DecoderStatus *s) {
    if (s->satellite == 2) {
        static const double esn0[] = {
            NAN, -2.35, -1.24, -.30, 1.00, 2.23, 3.10, 4.03, 4.68, 5.18, 6.20, 6.42,
            5.50, 6.62, 7.91, 9.35, 10.69, 10.98,
            8.97, 10.21, 11.03, 11.61, 12.89, 13.13,
            12.73, 13.64, 14.28, 15.69, 16.05
        };
        int mc=s->modcod;
        if (mc < 1 || mc > 28 || (s->short_frame != 0 && s->short_frame != 1)) return NAN;
        unsigned order=mc <= 11 ? 4 : mc <= 17 ? 8 : mc <= 23 ? 16 : 32;
        if (s->constellation_order != order) return NAN;
        /* DVB-S2 has no short-frame 9/10 code. */
        if (s->short_frame && (mc == 11 || mc == 17 || mc == 23 || mc == 28)) return NAN;
        return esn0[mc] + (s->short_frame ? .3 : 0);
    }
    if (s->satellite == 1 && s->constellation_order == 4) {
        static const struct { const char *fec; double rate, ebn0; } rates[] = {
            {"1/2", 1./2, 4.5}, {"2/3", 2./3, 5.0}, {"3/4", 3./4, 5.5},
            {"5/6", 5./6, 6.0}, {"7/8", 7./8, 6.4}
        };
        for (unsigned i=0; i<sizeof(rates)/sizeof(rates[0]); ++i)
            if (!strcmp(s->fec,rates[i].fec))
                return rates[i].ebn0 + 10*log10(2*rates[i].rate*188/204);
    }
    return NAN;
}

static inline double decoder_margin_db(const DecoderStatus *s) {
    /* PL/sync lock suffices: requiring FEC/TS lock would hide negative margin.
     * The caller must additionally check telemetry freshness and selected mode. */
    if (!(s->lock_valid & DECODER_OFDM) || !(s->locks & DECODER_OFDM) || !isfinite(s->mer_db)) return NAN;
    return s->mer_db - decoder_margin_reference(s);
}
#endif
