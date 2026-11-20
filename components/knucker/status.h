#ifndef LINRAD_KNUCKER_STATUS_H
#define LINRAD_KNUCKER_STATUS_H

#include "../../src/decoder_monitor.h"
#include <glib.h>
#include <math.h>
#include <stdio.h>

/* A complete line comes from our local receiver worker. Lock is the tuner
 * register, while TS activity counts aligned packets in this poll interval. */
static gboolean knucker_parse_status(const char *line, DecoderStatus *out, guint64 *sync_bytes) {
    unsigned lock;
    unsigned long long packets, delta, discarded;
    char snr[24], fft[8], constellation[12], fec[12], guard[12];
    if (sscanf(line,"[knucker] lock=%u snr=%23s fft=%7s constellation=%11s fec=%11s gi=%11s packets=%llu delta=%llu sync_discarded_bytes=%llu",
        &lock,snr,fft,constellation,fec,guard,&packets,&delta,&discarded)!=9) return FALSE;
    DecoderStatus s={.magic=DECODER_MONITOR_MAGIC,.type=DECODER_STATUS,
        .lock_valid=DECODER_OFDM|DECODER_TS,.snr_db=NAN,.pilot_lock=NAN,
        .rms_dbfs=NAN,.peak_dbfs=NAN,.cfo_hz=NAN,.plp_id=-1};
    if (lock==1) {
        s.locks|=DECODER_OFDM;
        s.snr_db=g_ascii_strtod(snr,NULL);
        g_snprintf(s.modulation,sizeof(s.modulation),"%s %s",constellation,fft);
        g_strlcpy(s.fec,fec,sizeof(s.fec));
        g_strlcpy(s.guard,guard,sizeof(s.guard));
    }
    if (delta) s.locks|=DECODER_TS;
    s.packets=s.written_packets=MIN(packets,G_MAXUINT32);
    *out=s;
    *sync_bytes=discarded;
    return TRUE;
}
/* BATC prints a lock transition followed by separate TPS and SNR lines. */
static gboolean batc_parse_status(const char *line, DecoderStatus *s, char fft[12], char constellation[12]) {
    if (!strcmp(line,"Unlocked") || !strcmp(line,"locked")) {
        s->locks=!strcmp(line,"locked") ? DECODER_OFDM : 0;
        s->lock_valid=DECODER_OFDM|DECODER_TS;
        s->snr_db=s->pilot_lock=s->rms_dbfs=s->peak_dbfs=s->cfo_hz=NAN;
        s->plp_id=-1;
        s->service[0]=s->provider[0]=0;
        fft[0]=constellation[0]=s->modulation[0]=s->guard[0]=s->fec[0]=0;
        return TRUE;
    }
    if (!(s->locks & DECODER_OFDM)) return FALSE;
    if (sscanf(line,"FFT : %11s",fft)==1 || sscanf(line,"Const: %11s",constellation)==1)
        g_snprintf(s->modulation,sizeof(s->modulation),"%s %s",constellation,fft);
    else if (g_str_has_prefix(line,"SNR is "))
        s->snr_db=g_ascii_strtod(line+7,NULL);
    else if (sscanf(line,"FEC : %15s",s->fec)!=1 && sscanf(line,"Guard: %15s",s->guard)!=1)
        return FALSE;
    return TRUE;
}
#endif
