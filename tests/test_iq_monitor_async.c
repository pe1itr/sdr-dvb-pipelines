#define _POSIX_C_SOURCE 200809L
/* Block the spectrum FFT deliberately: feeding IQ must still return, with
 * bounded spectrum-only loss, and shutdown must drain a pending snapshot. */
#include <assert.h>
#include <pthread.h>
#include <fftw3.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int entered, released;
static void slow_execute(fftwf_plan p) {
    pthread_mutex_lock(&gate);entered=1;pthread_cond_signal(&changed);
    while (!released) pthread_cond_wait(&changed,&gate);
    pthread_mutex_unlock(&gate);
    fftwf_execute(p);
}
#define fftwf_execute slow_execute
#include "../src/iq_monitor.c"
#undef fftwf_execute
int main(void) {
    setenv("LINRAD_IQ_MONITOR_PORT","9",1);
    setenv("LINRAD_IQ_MONITOR_SYNC","0",1);
    assert(iq_monitor_start_async()==1);
    unsigned char data[SNAPSHOT_SAMPLES*4], original[sizeof(data)];
    memset(data,0x5a,sizeof(data));memcpy(original,data,sizeof(data));
    iq_monitor_feed(data,SNAPSHOT_SAMPLES,0,163840);
    pthread_mutex_lock(&gate);
    while (!entered) pthread_cond_wait(&changed,&gate);
    pthread_mutex_unlock(&gate);
    for (int i=0;i<100;++i) iq_monitor_feed(data,SNAPSHOT_SAMPLES,0,163840);
    assert(iq_monitor_dropped()==99); /* one pending, one blocked in FFT */
    assert(!memcmp(data,original,sizeof(data)));
    pthread_mutex_lock(&gate);released=1;pthread_cond_signal(&changed);pthread_mutex_unlock(&gate);
    iq_monitor_stop_async();
    assert(!monitor.pending);
    setenv("LINRAD_IQ_MONITOR_SYNC","1",1);assert(iq_monitor_start_async()==0);
    puts("Blocked spectrum worker leaves IQ unchanged and drops only bounded spectrum snapshots");
    return 0;
}
