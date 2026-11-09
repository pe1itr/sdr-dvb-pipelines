#include "../src/sdr_rx_status.h"
#include <assert.h>
#include <locale.h>
int main(void) {
    SdrRxTiming t={0};
    sdr_rx_begin(&t,1);sdr_rx_end(&t,1.002,65536,0,0);
    sdr_rx_begin(&t,1.010);sdr_rx_end(&t,1.011,0,1,0);
    sdr_rx_begin(&t,1.012);sdr_rx_end(&t,1.112,0,0,1);
    FILE *f=tmpfile();assert(f);
    sdr_rx_report(f,&t,9142857,1.25,262144,3,1);
    rewind(f);char line[1024];assert(fgets(line,sizeof(line),f));fclose(f);
    line[strcspn(line,"\n")]=0;
    SdrRxStatus s={0};assert(sdr_rx_status_parse(line,&s));
    assert(s.calls==3 && s.samples==65536 && s.overflows==1 && s.timeouts==1);
    assert(s.output_dropped==262144 && s.spectrum_dropped==3 && s.spectrum_async);
    assert(s.gaps_over_5ms==1 && fabs(s.gap_max_ms-8)<.001);
    assert(fabs(s.call_interval_max_ms-10)<.001 && fabs(s.overflow_gap_max_ms-8)<.001);
    assert(fabs(s.recv_mean_ms-34.333)<.001 && fabs(s.recv_max_ms-100)<.001);
    assert(t.window_calls==0 && t.status.gap_max_ms==0 && t.status.calls==3);
    /* Lifetime counters persist; interval peaks clear; first call after report
     * still accounts for the preceding outside-recv interval. */
    sdr_rx_begin(&t,1.116);sdr_rx_end(&t,1.118,42,0,0);
    assert(t.status.calls==4 && t.status.samples==65578 && fabs(t.status.gap_max_ms-4)<.001);
    char prefix[1100];snprintf(prefix,sizeof(prefix),"OO%s",line);
    assert(sdr_rx_status_parse(prefix,&s));
    /* No partial/malformed line may replace the last valid snapshot. */
    SdrRxStatus previous=s;
    assert(!sdr_rx_status_parse("[SDR] RX timing: calls=2",&s));
    assert(!memcmp(&s,&previous,sizeof(s)));
    strcat(line," garbage");assert(!sdr_rx_status_parse(line,&s));
    assert(!sdr_rx_status_parse("[SDR] RX timing: calls=-1",&s));
    assert(!sdr_rx_status_parse("[SDR] RX timing: calls=18446744073709551616",&s));
    setlocale(LC_NUMERIC, "nl_NL.UTF-8");
    assert(sdr_rx_status_parse(prefix,&s));
    puts("SDR timing counters, interval resets and locale-independent parsing passed");
    return 0;
}
