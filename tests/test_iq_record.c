#define _POSIX_C_SOURCE 200809L
#include "../src/iq_record.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char directory[] = "/tmp/iq-record-test-XXXXXX";
    assert(mkdtemp(directory)); assert(!chdir(directory));
    IQRecord record = {.rate=250000};
    const char *path = "linrad-dvb-test-144600000-250000.iq";
    iq_record_command(&record, "record 144600000 linrad-dvb-test-144600000-250000.iq");
    assert(record.output);
    const unsigned char signed8[] = {0x80, 0x7f, 0xff, 0, 1, 0xfe};
    const unsigned char unsigned8[] = {0, 255, 127, 128, 129, 126};
    const unsigned char expected[] = {0, 0x80, 0, 0x7f, 0, 0xff, 0, 0, 0, 1, 0, 0xfe};
    iq_record_feed(&record, signed8, 3, 2);
    iq_record_feed(&record, unsigned8, 3, 1);
    iq_record_feed(&record, expected, 3, 0);
    /* Stop immediately: the writer must drain every queued sample. */
    iq_record_stop(&record); assert(!record.output);
    FILE *file = fopen(path, "rb"); assert(file);
    unsigned char actual[sizeof(expected)];
    for (int i = 0; i < 3; ++i) {
        assert(fread(actual, 1, sizeof(actual), file) == sizeof(actual));
        assert(!memcmp(actual, expected, sizeof(actual)));
    }
    assert(fgetc(file) == EOF); fclose(file);
    iq_record_command(&record, "record 144600000 linrad-dvb-test-144600000-250000.iq");
    assert(!record.output); /* Existing files must survive. */
    assert(!unlink(path));
    assert(!unlink("linrad-dvb-test-144600000-250000.iq.json"));
    /* Exclusive open rejects links; a disk-write error stops only recording. */
    assert(!symlink("/dev/full", path));
    iq_record_command(&record, "record 144600000 linrad-dvb-test-144600000-250000.iq");
    assert(!record.output); assert(!unlink(path));
    record.fd = open("/dev/full", O_WRONLY); assert(record.fd >= 0);
    record.output = iq_output_create(record.fd, 1024, 1); assert(record.output);
    iq_record_feed(&record, expected, 3, 0);
    iq_record_stop(&record); assert(!record.output);
    assert(!chdir("/tmp")); assert(!rmdir(directory));
    puts("IQ recording: signed/unsigned conversion, ordering, drain, overwrite protection and write errors passed");
    return 0;
}
