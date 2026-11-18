#ifndef DVBT_BACKEND_H
#define DVBT_BACKEND_H
#include <stddef.h>
#include "decoder_monitor.h"
void dvbt_monitor_health(DecoderDvbtHealth *frame);
#include "dvbt_outer.h"
int dvbt_backend_main(int argc, char **argv);
int dvbt_monitor_enabled(void);
void dvbt_thread_name(const char *name);
void dvbt_monitor_buffer(unsigned queued, unsigned capacity);
void dvbt_monitor_constellation(const float *iq, size_t count);
void dvbt_monitor_status(const rbdvbt_status_context_t *status,
                         const char *service, const char *provider,
                         unsigned locks, unsigned packets, unsigned written,
                         unsigned rs_ok, unsigned rs_bad, unsigned cc_errors);
#endif
