#ifndef IQ_OUTPUT_H
#define IQ_OUTPUT_H
#include <stddef.h>
#include <stdint.h>
typedef struct IQOutput IQOutput;
typedef struct {
    uint64_t accepted, written, dropped;
    size_t queued, capacity, peak;
} IQOutputStats;
void iq_output_stats(IQOutput *q, IQOutputStats *stats);
/* A bounded byte queue (capacity = block_bytes * slots): push never waits for downstream I/O. */
IQOutput *iq_output_create(int fd, size_t block_bytes, size_t slots);
/* 0 = queued, 1 = full (this input block dropped, existing queue preserved), -1 = writer failed. */
int iq_output_push(IQOutput *q, const void *data, size_t bytes);
int iq_output_error(IQOutput *q);
void iq_output_destroy(IQOutput *q);
#endif
