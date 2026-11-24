#ifndef IQ_RECORD_H
#define IQ_RECORD_H
#include "iq_output.h"
#include <stdio.h>
typedef struct {
    IQOutput *output;
    int fd;
    double rate, frequency;
    FILE *metadata;
    char path[192];
} IQRecord;
/* Commands: record FREQUENCY_HZ BASENAME, record-stop. Status goes to stderr. */
void iq_record_command(void *data, const char *line);
/* Full-band samples, before the decoder filter; format 0=s16le, 1=u8, 2=s8. */
void iq_record_feed(IQRecord *r, const void *data, size_t samples, int format);
void iq_record_stop(IQRecord *r);
#endif
