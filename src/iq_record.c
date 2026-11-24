#define _POSIX_C_SOURCE 200809L
#include "iq_record.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int write_metadata(IQRecord *r, size_t first_block) {
    if (!r->metadata) return 0;
    char stamp[80]="";
    struct timespec now;
    if (first_block && clock_gettime(CLOCK_REALTIME,&now)==0) {
        long long us=(long long)now.tv_sec*1000000 + now.tv_nsec/1000 -
            llround(first_block*1e6/r->rate);
        snprintf(stamp,sizeof(stamp),",\"capture_start_unix_us\":%lld",us);
    }
    int failed=fprintf(r->metadata,"{\"format\":\"cs16_le\",\"sample_rate_hz\":%.0f,"
        "\"center_frequency_hz\":%.0f,\"header_bytes\":0%s}\n",r->rate,r->frequency,stamp)<0;
    int error=failed ? errno : 0;
    if (fclose(r->metadata) && !error) error=errno;
    r->metadata=NULL;
    return error;
}

static void finish(IQRecord *r, int error) {
    int meta_error=write_metadata(r,0);
    if (!error) error=meta_error;
    if (r->output) {
        int drained = iq_output_finish(r->output);
        r->output = NULL;
        if (!error) error = drained;
        if (close(r->fd) && !error) error = errno;
    }
    if (error) fprintf(stderr, "[iq-record] error %s: %s\n", r->path, strerror(error));
    else fprintf(stderr, "[iq-record] stopped %s\n", r->path);
}

void iq_record_stop(IQRecord *r) {
    if (r->output) finish(r, 0);
}

void iq_record_command(void *data, const char *line) {
    IQRecord *r = data;
    if (!strcmp(line, "record-stop")) { finish(r, 0); return; }
    double frequency;
    char path[192], extra;
    if (sscanf(line, "record %lf %191s %c", &frequency, path, &extra) != 2 ||
        !isfinite(frequency) || frequency < 0 || !isfinite(r->rate) || r->rate <= 0 ||
        strncmp(path, "linrad-dvb-", 11) || strspn(path, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._") != strlen(path)) {
        fprintf(stderr, "[iq-record] error Invalid recording command\n"); return;
    }
    if (r->output) { fprintf(stderr, "[iq-record] started %s\n", r->path); return; }
    snprintf(r->path, sizeof(r->path), "%s", path);
    r->fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (r->fd < 0) { finish(r, errno); return; }
    /* Bounded half-second buffer: receive blocks queue without waiting for disk I/O.
     * Stop explicitly on overflow rather than silently producing gaps. */
    size_t slots = (size_t)ceil(r->rate * .5 / 65536);
    r->output = iq_output_create(r->fd, 65536 * 4, slots);
    if (!r->output) { int error = errno; close(r->fd); finish(r, error); return; }
    char metadata[sizeof(r->path) + 6];
    snprintf(metadata, sizeof(metadata), "%s.json", path);
    r->frequency=frequency;
    r->metadata=fopen(metadata,"wx");
    if (!r->metadata) { finish(r,errno); return; }
    fprintf(stderr, "[iq-record] started %s\n", path);
}

void iq_record_feed(IQRecord *r, const void *data, size_t samples, int format) {
    if (!r->output) return;
    int error = samples && r->metadata ? write_metadata(r,samples) : 0;
    if (!error) error = iq_output_error(r->output);
    if (error) { finish(r, error); return; }
    const unsigned char *input = data;
    unsigned char converted[65536 * 4];
    while (samples && r->output) {
        size_t count = samples < 65536 ? samples : 65536;
        const void *out = input;
        if (format) {
            for (size_t i = 0; i < count * 2; ++i) {
                int v = format == 1 ? (int)input[i] - 128 : (input[i] < 128 ? input[i] : (int)input[i] - 256);
                uint16_t word = (uint16_t)(v * 256);
                converted[2*i] = word & 255; converted[2*i+1] = word >> 8;
            }
            out = converted;
        }
        int queued = iq_output_push(r->output, out, count * 4);
        if (queued) {
            int error = queued > 0 ? ENOBUFS : iq_output_error(r->output);
            finish(r, error ? error : EIO); break;
        }
        input += count * (format ? 2 : 4); samples -= count;
    }
}
